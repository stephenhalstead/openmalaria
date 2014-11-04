/* This file is part of OpenMalaria.
 * 
 * Copyright (C) 2005-2014 Swiss Tropical and Public Health Institute
 * Copyright (C) 2005-2014 Liverpool School Of Tropical Medicine
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

#include "WithinHost/Infection/CommonInfection.h"
#include "util/random.h"
#include "util/errors.h"
#include "schema/scenario.h"

#include <boost/format.hpp>

namespace OM {
namespace WithinHost {

namespace GT /*for genotype impl details*/{
enum SampleMode{
    SAMPLE_FIRST,      // always choose first genotype (essentially the off switch)
    SAMPLE_INITIAL,    // sample from initial probabilities
    SAMPLE_TRACKING    // sample from tracked success at genotype level (no recombination)
};
// Mode to use now (until switched) and from the start of the intervention period.
SampleMode current_mode = SAMPLE_FIRST, inerv_mode = SAMPLE_FIRST;

// keys are cumulative probabilities; last entry should equal 1; values are genotype codes
map<double,uint32_t> cum_initial_freqs;

// we give each allele of each loci a unique code
map<string, map<string, uint32_t> > alleleCodes;
uint32_t nextAlleleCode = 0;

// Represent a set of loci: all possible combinations of alleles
// This is actually just machinery to calculate the list of all genotypes
struct LocusSet{
    LocusSet( const scnXml::ParasiteLocus& elt_l ){
        alleles.reserve( elt_l.getAllele().size() );
        double cum_p = 0.0;
        foreach( const scnXml::ParasiteAllele& elt_a, elt_l.getAllele() ){
            uint32_t alleleCode = (nextAlleleCode++);
            alleleCodes[elt_l.getName()][elt_a.getName()] = alleleCode;
            cum_p += elt_a.getInitialFrequency();
            alleles.push_back( Genotype::AlleleComb( alleleCode, elt_a.getInitialFrequency(), elt_a.getFitness() ) );
        }
        if( cum_p < 0.999 || cum_p > 1.001 ){
            throw util::xml_scenario_error( (
                boost::format("expected sum of initial probabilities of "
                "alleles to be 1, but for the %1% alleles under locus %2% "
                "this is %3%") %alleles.size() %elt_l.getName() %cum_p
            ).str() );
        }
        alleles[0].init_freq += 1.0 - cum_p;     // account for any small errors by adjusting the first frequency
    }
    void include( LocusSet& that ){
        vector<Genotype::AlleleComb> newAlleles;
        newAlleles.reserve( alleles.size() * that.alleles.size() );
        for( size_t i = 0; i < alleles.size(); i += 1 ){
            for( size_t j = 0; j < that.alleles.size(); j += 1 ){
                // note that we generate new codes each time; we just waste the old ones
                newAlleles.push_back( alleles[i].cross(that.alleles[j]) );
            }
        }
        alleles.swap(newAlleles);
    }
    
    vector<Genotype::AlleleComb> alleles;
};

vector<Genotype::AlleleComb> genotypes;
}

Genotype::AlleleComb Genotype::AlleleComb::cross( const AlleleComb& that )const{
    AlleleComb result(*that.alleles.begin(), init_freq*that.init_freq, fitness*that.fitness);
    result.alleles.insert( alleles.begin(), alleles.end() );
    result.alleles.insert( that.alleles.begin(), that.alleles.end() );
    return result;
}

void Genotype::initSingle()
{
    // no specification implies there is a single genotype
    GT::genotypes.assign( 1, Genotype::AlleleComb(
        0 /*allele code*/, 1.0/*frequency*/, 1.0/*fitness*/) );
}

void Genotype::init( const scnXml::Scenario& scenario ){
    if( scenario.getParasiteGenotypology().present() ){
        const scnXml::ParasiteGenotypology& genotypology =
            scenario.getParasiteGenotypology().get();
        
        GT::current_mode = GT::SAMPLE_INITIAL;      // turn on sampling
        if( genotypology.getSamplingMode() == "initial" ){
            GT::inerv_mode = GT::SAMPLE_INITIAL;
        }else if( genotypology.getSamplingMode() == "tracking" ){
            GT::inerv_mode = GT::SAMPLE_TRACKING;
        }else{
            throw util::xml_scenario_error( "parasiteGenotypology/samplingMode: expected \"initial\" or \"tracking\"" );
        }
        
        // Build the list of all allele combinations by iterating over loci (plural of locus):
        GT::LocusSet loci( genotypology.getLocus()[0] );
        for( size_t i = 1; i < genotypology.getLocus().size(); i += 1 ){
            GT::LocusSet newLocus(genotypology.getLocus()[i]);
            loci.include( newLocus );
        }
        GT::genotypes.swap( loci.alleles );
        
        double cum_p = 0.0;
        for( size_t i = 0; i < GT::genotypes.size(); ++i ){
            cum_p += GT::genotypes[i].init_freq;
            uint32_t genotype_id = static_cast<uint32_t>(i);
            assert( genotype_id == i );
            GT::cum_initial_freqs.insert( make_pair(cum_p, genotype_id) );
        }
        
        // Test cum_p is approx. 1.0 in case the input tree is wrong. We require no
        // less than one to make sure generated random numbers are not greater than
        // the last option.
        if (cum_p < 1.0 || cum_p > 1.001){
            throw util::xml_scenario_error ( (
                boost::format("decision tree (random node): expected probability sum to be 1.0 but found %2%")
                %cum_p
            ).str() );
        }
    }else{
        initSingle();
    }
}

uint32_t Genotype::findAlleleCode(const string& locus, const string& allele){
    map<string, map<string, uint32_t> >::const_iterator it = GT::alleleCodes.find(locus);
    if( it == GT::alleleCodes.end() ) return numeric_limits<uint32_t>::max();
    map<string, uint32_t>::const_iterator it2 = it->second.find( allele );
    if( it2 == it->second.end() ) return numeric_limits<uint32_t>::max();
    return it2->second;
}

const vector< Genotype::AlleleComb >& Genotype::getGenotypes(){
    return GT::genotypes;
}

uint32_t Genotype::sampleGenotype(){
    if( GT::current_mode == GT::SAMPLE_FIRST ){
        return 0;       // always the first genotype code
    }else if( GT::current_mode == GT::SAMPLE_INITIAL ){
        double sample = util::random::uniform_01();
        map<double,uint32_t>::const_iterator it = GT::cum_initial_freqs.upper_bound( sample );
        assert( it != GT::cum_initial_freqs.end() );
        return it->second;
    }else{
        assert(false);  //FIXME
    }
}

void CommonInfection::checkpoint(ostream& stream)
{
    OM::WithinHost::Infection::checkpoint(stream);
    m_genotype & stream;
}
CommonInfection::CommonInfection(istream& stream):
    Infection(stream)
{
    m_genotype & stream;
}

}
}
