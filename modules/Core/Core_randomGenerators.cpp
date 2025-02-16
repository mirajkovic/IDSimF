/***************************
 Ion Dynamics Simulation Framework (IDSimF)

 Copyright 2020 - Physical and Theoretical Chemistry /
 Institute of Pure and Applied Mass Spectrometry
 of the University of Wuppertal, Germany

 IDSimF is free software: you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation, either version 3 of the License, or
 (at your option) any later version.

 IDSimF is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with IDSimF.  If not, see <https://www.gnu.org/licenses/>.
 ****************************/

#include "Core_randomGenerators.hpp"
#include <omp.h>
#include <iostream>

std::random_device Core::rdSeed; //seed generator

std::unique_ptr<Core::AbstractRandomGeneratorPool> Core::globalRandomGeneratorPool =
        std::make_unique<Core::RandomGeneratorPool>();

/**
 * Creates a mersenne bit source (initialized by the global seed generator)
 */
Core::MersenneBitSource::MersenneBitSource():
internalRandomSource(Core::rdSeed())
{}

/**
 * Sets the random seed of the bit source
 */
void Core::MersenneBitSource::seed(Core::rndBit_type seed) {
    internalRandomSource.seed(seed);
}

/**
 * Maximum value of the value range
 */
/*Core::rndBit_type Core::MersenneBitSource::max() {
    return std::mt19937::max();
}*/

/**
 * Minimum value of the value range
 */
/*Core::rndBit_type Core::MersenneBitSource::min() {
    return std::mt19937::min();
}*/

/**
 * Gets a random bit value
 */
Core::rndBit_type Core::MersenneBitSource::operator()() {
    return internalRandomSource();
}

/**
 * Creates a test (essentially non random) bit source.
 */
Core::TestBitSource::TestBitSource():
sampleIndex_(0)
{}

/**
 * Maximum value of the value range
 */
/*Core::rndBit_type Core::TestBitSource::max() {
    return 4294967295;
}*/

/**
 * Minimum value of the value range
 */
/*Core::rndBit_type Core::TestBitSource::min() {
    return 0;
}*/

/**
 * Generates next value from the predefined, short, list of bit values / bit patterns
 */
Core::rndBit_type Core::TestBitSource::operator()() {
    sampleIndex_ = (sampleIndex_+1) % Core::UNIFORM_RANDOM_BITS.size();
    return Core::UNIFORM_RANDOM_BITS[sampleIndex_];
}

/**
 * Creates a bit source based on the SplitMix64 algorithm
 */
Core::SplitMix64BitSource::SplitMix64BitSource() : state_(Core::rdSeed()) {}

/**
 * Sets the random seed of the bit source for SplitMix64
 */
void Core::SplitMix64BitSource::seed(Core::rndBit_type seed) {
    state_ = seed;
}

/**
 * Generates next value from the state of the SplitMix64 algorithm
 */
Core::rndBit_type Core::SplitMix64BitSource::operator()() {
    Core::rndBit_type randomSource = (state_ += 0x9e3779b97f4a7c15uLL);
    randomSource = (randomSource ^ (randomSource >> 30)) * 0xbf58476d1ce4e5b9uLL;
    randomSource = (randomSource ^ (randomSource >> 27)) * 0x94d049bb133111ebuLL;
    return randomSource ^ (randomSource >> 31);
}

/**
 * Creates a test bit source with a predefined seed (not random!) based on the SplitMix64 algorithm
 */
Core::SplitMix64TestBitSource::SplitMix64TestBitSource() : state_(Core::defaultSeed) {}

/**
 * Generates next value from the state of the SplitMix64 algorithm (predefined sequence)
 * Reference: https://prng.di.unimi.it/splitmix64.c
 */
Core::rndBit_type Core::SplitMix64TestBitSource::operator()() {
    Core::rndBit_type randomSource = (state_ += 0x9e3779b97f4a7c15uLL);
    randomSource = (randomSource ^ (randomSource >> 30)) * 0xbf58476d1ce4e5b9uLL;
    randomSource = (randomSource ^ (randomSource >> 27)) * 0x94d049bb133111ebuLL;
    return randomSource ^ (randomSource >> 31);
}

/**
 * Creates a test bit source with a predefined seed (not random!) based on the Xoshiro256+ algorithm 
 * with SplitMix64 seeding as recommended 
 */
Core::Xoshiro256pTestBitSource::Xoshiro256pTestBitSource() {
    SplitMix64TestBitSource splitmix64 = SplitMix64TestBitSource();
    std::array<rndBit_type, 4> s = {splitmix64(), splitmix64(), splitmix64(), splitmix64()};
    internalRandomSource = Core::Xoshiro256p(s);
}

/**
 * Generates next value from the state of the Xoshiro256+ algorithm (predefined sequence)
 */
Core::rndBit_type Core::Xoshiro256pTestBitSource::operator()() {
    return internalRandomSource();
}



/**
 * Construct a custom uniform random distribution with a custom interval
 * @param min lower boundary of the interval
 * @param max upper boundary of the interval
 */
Core::UniformRandomDistribution::UniformRandomDistribution(double min, double max,
                                                           Core::RandomBitSource<rndBit_type>* randomSource):
randomSource_(randomSource),
internalUniformDist_(min, max)
{}

/**
 * Generate random value in the uniform distribution
 * @return uniformly distributed random value
 */
double Core::UniformRandomDistribution::rndValue(){
    return internalUniformDist_(*randomSource_);
}


/**
 * Construct a custom test distribution with a custom interval
 * @param min lower boundary of the interval
 * @param max upper boundary of the interval
 */
Core::UniformTestDistribution::UniformTestDistribution(double min, double max):
sampleIndex_(0),
min_(min),
interval_(max-min)
{}

/**
 * Generate non random test value in the uniform distribution
 * @return uniformly distributed test value
 */
double Core::UniformTestDistribution::rndValue() {
    sampleIndex_ = (sampleIndex_+1) % Core::UNIFORM_TEST_SAMPLES.size();
    return min_ + Core::UNIFORM_TEST_SAMPLES[sampleIndex_]*interval_;
}


/**
 * Construct normal test distribution
 */
Core::NormalTestDistribution::NormalTestDistribution(): sampleIndex_(0)
{}

/**
 * Generate normal test value
 * @return non random normal distributed test value
 */
double Core::NormalTestDistribution::rndValue() {
    sampleIndex_ = (sampleIndex_+1) % Core::NORMAL_TEST_SAMPLES.size();
    return Core::NORMAL_TEST_SAMPLES[sampleIndex_];
}


/**
 * Construct uniform test distribution based on Xoshiro256+
 */
Core::UniformTestDistributionXoshiro::UniformTestDistributionXoshiro(Xoshiro256pTestBitSource* randomSource){
    randomSource_ = randomSource;
}

/**
 * Construct a custom test distribution with a custom interval based on Xoshiro256+
 * @param min lower boundary of the interval
 * @param max upper boundary of the interval
 */
Core::UniformTestDistributionXoshiro::UniformTestDistributionXoshiro(double min, double max, Xoshiro256pTestBitSource* randomSource):
randomSource_(randomSource),
min_(min),
interval_(max-min)
{}

/**
 * Generate non random test value in the uniform distribution from the Xoshiro256+ algorithm 
 * @return uniformly distributed test value
 */
double Core::UniformTestDistributionXoshiro::rndValue() {
    rndBit_type x = randomSource_->internalRandomSource();
    const union { rndBit_type i; double d; } u = {UINT64_C(0x3FF) << 52 | x >> 12 };
    return (u.d - 1.0) * interval_ + min_;
}

/**
 * Construct normal test distribution based on Xoshiro256+
 */
Core::NormalTestDistributionXoshiro::NormalTestDistributionXoshiro(Xoshiro256pTestBitSource* randomSource): randomSource_(randomSource)
{}

/**
 * Generate normal test value by Box-Muller transform from Xoshiro256+ generated uniform numbers
 * Reference 1: https://en.wikipedia.org/wiki/Box%E2%80%93Muller_transform
 * Reference 2: https://prng.di.unimi.it/
 * @return non random normal distributed test value
 */
double Core::NormalTestDistributionXoshiro::rndValue() {
    rndBit_type x = randomSource_->internalRandomSource();
    rndBit_type y = randomSource_->internalRandomSource();
    const union { rndBit_type i; double d; } u = {UINT64_C(0x3FF) << 52 | x >> 12 };
    const union { rndBit_type i; double d; } v = {UINT64_C(0x3FF) << 52 | y >> 12 };

    double u1 = (u.d - 1.0);
    double v1 = (v.d - 1.0);

    double a = sqrt(-2 * log(u1)) * cos(2 * 3.141592653 * v1);
    double b = sqrt(-2 * log(u1)) * sin(2 * 3.141592653 * v1);

    return a; // return either a or b 
}


/**
 * Sets the random seed of the random generator pool element
 */
void Core::RandomGeneratorPool::RNGPoolElement::seed(Core::rndBit_type seed) {
    rngGenerator_.internalRandomSource.seed(seed);
}

/**
 * Get uniformly distributed random value
 * @return random value, uniformly distrbuted in the interval [0, 1)
 */
double Core::RandomGeneratorPool::RNGPoolElement::uniformRealRndValue() {
    return uniformDist_(rngGenerator_.internalRandomSource);
}

/**
 * Get normal distributed random value
 * @return random value, normally distributed
 */
double Core::RandomGeneratorPool::RNGPoolElement::normalRealRndValue() {
    return normalDist_(rngGenerator_.internalRandomSource);
}

/**
 * Gets the random bit source of this random source
 * @return A random bit source, based on the mersenne twister
 */
Core::MersenneBitSource* Core::RandomGeneratorPool::RNGPoolElement::getRandomBitSource() {
    return &rngGenerator_;
}

/**
 * Constructs the productive random generator pool
 */
Core::RandomGeneratorPool::RandomGeneratorPool() {
    int nMaxThreads_ = omp_get_max_threads();
    for (int i=0; i<nMaxThreads_; ++i){
        elements_.emplace_back(std::make_unique<Core::RandomGeneratorPool::RNGPoolElement>());
    }
}

/**
 * Sets a new random seed for all elements of the pools
 */
void Core::RandomGeneratorPool::setSeedForElements(Core::rndBit_type newSeed) {
    for (int i=0; i<elements_.size(); ++i) {
        elements_[i]->seed(newSeed);
    }
}

/**
 * Get a new uniform random distribution in the interval [min, max]. Note that the underlying random bit source
 * is the random bit source associated to *the current* thread. The random bit source is NOT changed if the
 * distribution is called from another thread.
 *
 * @param min Lower boundary of the interval of the random values
 * @param max Upper boundary of the interval of the random values
 * @return A new uniform random distribution in the interval [min, max)
 */
Core::RndDistPtr Core::RandomGeneratorPool::getUniformDistribution(double min, double max) {
    return std::make_unique<Core::UniformRandomDistribution>(min, max, this->getThreadRandomSource()->getRandomBitSource());
}

/**
 * Gets the random source associated to the current thread
 * @return A random source, which is associated to the current thread
 */
Core::RandomGeneratorPool::RNGPoolElement* Core::RandomGeneratorPool::getThreadRandomSource() {
    return elements_[static_cast<std::size_t>(omp_get_thread_num())].get();
}

/**
 * Gets the random source associated to a thread, specified by an index
 * @param index The index of the random source
 * @return A random source, which is associated the specified thread
 */
Core::RandomGeneratorPool::RNGPoolElement* Core::RandomGeneratorPool::getRandomSource(std::size_t index) {
    return elements_.at(index).get();
}


/**
* Get uniformly distributed test value from a short list of predefined, uniformly distributed values
* @return test value, uniformly distributed in the interval [0, 1)
*/
double Core::TestRandomGeneratorPool::TestRNGPoolElement::uniformRealRndValue() {
    return uniformDist_.rndValue();
}

/**
 * Get normal distributed test value from a short list of predefined, normally distrbuted values
 *
 * @return test value, normally distributed
 */
double Core::TestRandomGeneratorPool::TestRNGPoolElement::normalRealRndValue() {
    return normalDist_.rndValue();
}

/**
 * Gets the test bit source of this test random source
 * @return A random bit source, based on the mersenne twister
 */
Core::TestBitSource* Core::TestRandomGeneratorPool::TestRNGPoolElement::getRandomBitSource() {
    return &rngGenerator_;
}

/**
 * Sets new random seed (currently not implemented, does nothing!)
 */
void Core::TestRandomGeneratorPool::setSeedForElements(Core::rndBit_type newSeed) {

}

/**
 * Get a new uniform test distribution in the interval [min, max).
 *
 * @param min Lower boundary of the interval of the random values
 * @param max Upper boundary of the interval of the random values
 * @return A new uniform test distribution in the interval [min, max)
 */
Core::RndDistPtr Core::TestRandomGeneratorPool::getUniformDistribution(double min, double max) {
    return std::make_unique<Core::UniformTestDistribution>(min, max);
}

/**
 * Gets a test random source for the current thread
 * @return Test random source
 */
Core::TestRandomGeneratorPool::TestRNGPoolElement* Core::TestRandomGeneratorPool::getThreadRandomSource() {
    return &element_;
}

/**
 * Gets a test random source for a specified thread
 * @return Test random source
 */
Core::TestRandomGeneratorPool::TestRNGPoolElement* Core::TestRandomGeneratorPool::getRandomSource(std::size_t) {
    return &element_;
}


Core::XoshiroTestRandomGeneratorPool::XoshiroTestRNGPoolElement::XoshiroTestRNGPoolElement() 
    : rngGenerator_(), uniformDist_(&rngGenerator_), normalDist_(&rngGenerator_)
{}

/**
 * Get uniformly distributed random value
 * @return random value, uniformly distrbuted in the interval [0, 1)
 */
double Core::XoshiroTestRandomGeneratorPool::XoshiroTestRNGPoolElement::uniformRealRndValue() {
    return uniformDist_.rndValue();
}

/**
 * Get normal distributed random value
 * @return random value, normally distributed
 */
double Core::XoshiroTestRandomGeneratorPool::XoshiroTestRNGPoolElement::normalRealRndValue() {
    return normalDist_.rndValue();
}

/**
 * Gets the random bit source of this random source
 * @return A random bit source, based on the test xoshiro256+
 */
Core::Xoshiro256pTestBitSource* Core::XoshiroTestRandomGeneratorPool::XoshiroTestRNGPoolElement::getRandomBitSource() {
    return &rngGenerator_;
}

/**
 * Sets a new random seed for all elements of the pools
 */
void Core::XoshiroTestRandomGeneratorPool::setSeedForElements(Core::rndBit_type newSeed) {
    // to-do;
}

/**
 * Get a new uniform random distribution in the interval [min, max). Note that the underlying random bit source
 * is the random bit source associated to *the current* thread. The random bit source is NOT changed if the
 * distribution is called from another thread.
 *
 * @param min Lower boundary of the interval of the random values
 * @param max Upper boundary of the interval of the random values
 * @return A new uniform random distribution in the interval [min, max)
 */
Core::RndDistPtr Core::XoshiroTestRandomGeneratorPool::getUniformDistribution(double min, double max) {
    return std::make_unique<Core::UniformTestDistributionXoshiro>(min, max, this->getThreadRandomSource()->getRandomBitSource());
}

/**
 * Gets a test random source for the current thread
 * @return Test random source
 */
Core::XoshiroTestRandomGeneratorPool::XoshiroTestRNGPoolElement* Core::XoshiroTestRandomGeneratorPool::getThreadRandomSource() {
    return &element_;
}

/**
 * Gets a test random source for a specified thread
 * @return Test random source
 */
Core::XoshiroTestRandomGeneratorPool::XoshiroTestRNGPoolElement* Core::XoshiroTestRandomGeneratorPool::getRandomSource(std::size_t index) {
    return &element_;
}

/**
 * @brief Bit rotation function for use in xoshiro256+
 * 
 * @param x number to rotate
 * @param k rotation number
 * @return rotated number  
 */
static inline Core::rndBit_type rotl(const Core::rndBit_type x, int k) {
	return (x << k) | (x >> (64 - k));
}

/**
 * @brief Initlaizes Xoshiro256+ algorithm with seed
 * 
 * @param seed input seed 
*/
Core::Xoshiro256p::Xoshiro256p(Core::rndBit_type seed){
    SplitMix64BitSource splitmix64 = SplitMix64BitSource();
    splitmix64.seed(seed);
    std::array<Core::rndBit_type, 4> s = {splitmix64(), splitmix64(), splitmix64(), splitmix64()};
    internalState_ = s; 
}

/**
 * @brief Initlaizes Xoshiro256+ algorithm with state
 * 
 * @param seed input state 
*/
Core::Xoshiro256p::Xoshiro256p(std::array<Core::rndBit_type, 4> state){
    internalState_ = state; 
}

/**
 * Generates next random number from xoshiro state 
 * reference: https://prng.di.unimi.it/xoshiro256plus.c
*/
Core::rndBit_type Core::Xoshiro256p::operator()(){
    const Core::rndBit_type result = internalState_[0] + internalState_[3];

	const Core::rndBit_type t = internalState_[1] << 17;

	internalState_[2] ^= internalState_[0];
	internalState_[3] ^= internalState_[1];
	internalState_[1] ^= internalState_[2];
	internalState_[0] ^= internalState_[3];

	internalState_[2] ^= t;

	internalState_[3] = rotl(internalState_[3], 45);
	return result;
}

/**
 * returns minimal bit source value 
*/
constexpr Core::rndBit_type Core::Xoshiro256p::min(){
    return Core::rndBit_type(0);
}

/**
 * returns max bit source value 
*/
constexpr Core::rndBit_type Core::Xoshiro256p::max(){
    return Core::rndBit_type(-1);
}