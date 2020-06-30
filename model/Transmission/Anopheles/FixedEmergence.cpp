/* This file is part of OpenMalaria.
 * 
 * Copyright (C) 2005-2015 Swiss Tropical and Public Health Institute
 * Copyright (C) 2005-2015 Liverpool School Of Tropical Medicine
 * 
 * OpenMalaria is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

#include "Transmission/Anopheles/FixedEmergence.h"
#include "Transmission/Anopheles/MosqTransmission.h"
#include "Transmission/Anopheles/Nv0DelayFitting.h"

#include "util/vectors.h"
#include "util/CommandLine.h"
#include "util/errors.h"

namespace OM {
namespace Transmission {
namespace Anopheles {
using namespace OM::util;


// -----  Initialisation of model, done before human warmup  ------

FixedEmergence::FixedEmergence() :
        initNv0FromSv(numeric_limits<double>::signaling_NaN())
{
    quinquennialS_v.assign (SimTime::fromYearsI(5), 0.0);
    mosqEmergeRate.resize (SimTime::oneYear()); // Only needs to be done here if loading from checkpoint
}


// -----  Initialisation of model which is done after creating initial humans  -----

void FixedEmergence::init2( double tsP_A, double tsP_df, double tsP_dff, double EIRtoS_v, MosqTransmission& transmission ){
    // -----  Calculate required S_v based on desired EIR  -----
    
    initNv0FromSv = initNvFromSv * (1.0 - tsP_A - tsP_df);

    // We scale FSCoeffic to give us S_v instead of EIR.
    // Log-values: adding log is same as exponentiating, multiplying and taking
    // the log again.
    FSCoeffic[0] += log( EIRtoS_v);
    vectors::expIDFT (forcedS_v, FSCoeffic, EIRRotateAngle);
    //vectors::expIDFT (forcedS_v, FSCoeffic, FSRotateAngle);
    
    transmission.initState ( tsP_A, tsP_df, tsP_dff, initNvFromSv, initOvFromSv, forcedS_v );
    
    // Crude estimate of mosqEmergeRate: (1 - P_A(t) - P_df(t)) / (T * ρ_S) * S_T(t)
    mosqEmergeRate = forcedS_v;
    vectors::scale (mosqEmergeRate, initNv0FromSv);
    
    // All set up to drive simulation from forcedS_v

    scaleFactor = 1.0;
    shiftAngle = 0;
}


// -----  Initialisation of model which is done after running the human warmup  -----
// int argmax(const vecDay<double> &vec)
// {
//     int imax = 0;
//     double max = 0.0;
//     for(int i=0; i<vec.size().inDays(); i++)
//     {
//         double v = vec[SimTime::fromDays(i)];
//         if(v >= max)
//         {
//             max = v;
//             imax = i;
//         }
//     }
//     return imax;
// }

double findAngle(double EIRRotageAngle, const vector<double> & FSCoeffic, vecDay<double> &sim)
{
    vecDay<double> temp(sim.size(), 0.0);

    double delta = 2 * M_PI / 365.0;

    double min = std::numeric_limits<double>::infinity();
    double minAngle = 0.0;
    for(double angle=-M_PI; angle<M_PI; angle+=delta)
    {
        vectors::expIDFT(temp, FSCoeffic, EIRRotageAngle + angle);

        // Minimize l1-norm
        double sum = 0.0;
        for(SimTime i=SimTime::zero(); i<SimTime::oneYear(); i+=SimTime::oneDay())
            sum += fabs(temp[i] - sim[i]);

        if(sum < min)
        {
            min = sum;
            minAngle = angle;
        }

        // // Or minimize peaks offset
        // int m1 = argmax(temp);
        // int m2 = argmax(sim);
        // int offset = abs(m1-m2);

        // if(offset < min)
        // {
        //     min = offset;
        //     minAngle = angle;
        // }

    }
    return minAngle;
}

bool FixedEmergence::initIterate (MosqTransmission& transmission) {
    // Try to match S_v against its predicted value. Don't try with N_v or O_v
    // because the predictions will change - would be chasing a moving target!
    // EIR comes directly from S_v, so should fit after we're done.

    // Compute avgAnnualS_v from quinquennialS_v for fitting 
    vecDay<double> avgAnnualS_v( SimTime::oneYear(), 0.0 );
    for( SimTime i = SimTime::zero(); i < SimTime::fromYearsI(5); i += SimTime::oneDay() ){
        avgAnnualS_v[mod_nn(i, SimTime::oneYear())] +=
            quinquennialS_v[i] / 5.0;
    }

    double factor = vectors::sum(forcedS_v) / vectors::sum(avgAnnualS_v);

    //cout << "Pre-calced Sv, dynamic Sv:\t"<<sumAnnualForcedS_v<<'\t'<<vectors::sum(annualS_v)<<endl;
    if (!(factor > 1e-6 && factor < 1e6)) {
        if( factor > 1e6 && vectors::sum(quinquennialS_v) < 1e-3 ){
            throw util::base_exception("Simulated S_v is approx 0 (i.e.\
 mosquitoes are not infectious, before interventions). Simulator cannot handle this; perhaps\
 increase EIR or change the entomology model.", util::Error::VectorFitting);
        }
        if ( vectors::sum(forcedS_v) == 0.0 ) {
            return false;   // no EIR desired: nothing to do
        }
        cerr << "Input S_v for this vector:\t"<<vectors::sum(forcedS_v)<<endl;
        cerr << "Simulated S_v:\t\t\t"<<vectors::sum(quinquennialS_v)/5.0<<endl;
        throw TRACED_EXCEPTION ("factor out of bounds",util::Error::VectorFitting);
    }

    //double rAngle = Nv0DelayFitting::fit<double> (EIRRotateAngle, FSCoeffic, avgAnnualS_v.internal());
    // forced_sv -> mosqEmergRate -> shift + scale

    // Increase the factor by 60% of the difference to slow down (and improve) convergence
    double factorDiff = (scaleFactor * factor - scaleFactor) * .6;

    //cout << scaleFactor << " + " << factorDiff << " => ";
    
    scaleFactor += factorDiff;

    shiftAngle += findAngle(EIRRotateAngle, FSCoeffic, avgAnnualS_v);

    vectors::expIDFT(mosqEmergeRate, FSCoeffic, -shiftAngle);

    vectors::scale (mosqEmergeRate, scaleFactor * initNv0FromSv);

    transmission.initIterateScale (scaleFactor);
    //initNvFromSv *= scaleFactor;     //(not currently used)

    //cout << scaleFactor << ", " << "Factor: " << factor << endl;
    //cout << "scaleFactor " << scaleFactor << ", " << "Factor: " << factor << ", Angle: " << shiftAngle << ", Offset: " << offset << " | " << m1 << ", " << m2 << endl;

    // int m1 = argmax(forcedS_v);
    // int m2 = argmax(avgAnnualS_v);
    // int offset = m1-m2;

    //cout << "scaleFactor " << scaleFactor << ", " << "Factor: " << factor << ", Angle: " << shiftAngle << ", Offset: " << offset << " | " << m1 << ", " << m2 << endl;

    // 5% fitting error allowed
    const double LIMIT = 0.05;
    return (fabs(factor - 1.0) > LIMIT);// || (rAngle > LIMIT * 2*M_PI / sim::stepsPerYear());
}


double FixedEmergence::update( SimTime d0, double nOvipositing, double S_v ){
    // We use time at end of step (i.e. start + 1) in index:
    SimTime d5Year = mod_nn(d0 + SimTime::oneDay(), SimTime::fromYearsI(5));
    quinquennialS_v[d5Year] = S_v;
    
    // Get emergence at start of step:
    SimTime dYear1 = mod_nn(d0, SimTime::oneYear());
    // Simple model: fixed emergence scaled by larviciding
    return mosqEmergeRate[dYear1] * interventionSurvival();
}

void FixedEmergence::checkpoint (istream& stream){ (*this) & stream; }
void FixedEmergence::checkpoint (ostream& stream){ (*this) & stream; }

}
}
}
