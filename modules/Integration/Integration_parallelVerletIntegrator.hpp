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

 ------------
 BTree_parallelVerletIntegrator.hpp
 
 Parallel version of a Barnes Hut tree based ion trajectory integration
 (simple verlet trajectory integrator) including space charge

 ****************************/

//FIXME: General: Establish dedicated types for time and use size_t

#ifndef BTree_parallelVerletIntegrator_hpp
#define BTree_parallelVerletIntegrator_hpp

#include "Core_particle.hpp"
#include "Core_vector.hpp"
#include "BTree_parallelTree.hpp"
#include "Integration_abstractTimeIntegrator.hpp"
#include "CollisionModel_AbstractCollisionModel.hpp"
#include <vector>

namespace Integration{

    //std::function<Core::Vector(Core::Particle* particle, int particleIndex, Core::Tree& tree, double time, int timestep)> accelerationFctType;

    class ParallelVerletIntegrator: public AbstractTimeIntegrator {

        public:

            ParallelVerletIntegrator(
                    const std::vector<Core::Particle*>& particles,
                    accelerationFctType accelerationFunction,
                    timestepWriteFctType timestepWriteFunction = nullptr,
                    otherActionsFctType otherActionsFunction = nullptr,
                    AbstractTimeIntegrator::particleStartMonitoringFctType ionStartMonitoringFunction = nullptr,
                    CollisionModel::AbstractCollisionModel* collisionModel = nullptr
            );

            ParallelVerletIntegrator(
                    accelerationFctType accelerationFunction,
                    timestepWriteFctType timestepWriteFunction = nullptr,
                    otherActionsFctType otherActionsFunction = nullptr,
                    AbstractTimeIntegrator::particleStartMonitoringFctType ionStartMonitoringFunction = nullptr,
                    CollisionModel::AbstractCollisionModel* collisionModel = nullptr
            );

            void addParticle(Core::Particle* particle) override;
            void run(unsigned int nTimesteps, double dt) override;
            void runSingleStep(double dt) override;
            void finalizeSimulation() override;

    private:

        CollisionModel::AbstractCollisionModel* collisionModel_ = nullptr; ///< the gas collision model to perform while integrating

        accelerationFctType accelerationFunction_ = nullptr;   ///< function to calculate particle acceleration
        timestepWriteFctType timestepWriteFunction_ = nullptr; ///< function to export / write time step results
        otherActionsFctType otherActionsFunction_ = nullptr;   ///< function for arbitrary other actions in the simulation

        //internal variables for actual calculations:
        Core::Vector loc_min_ = Core::Vector(-1000,-1000,-1000); ///< currently hard coded lower corner of the sim. domain
        Core::Vector loc_max_ = Core::Vector( 1000, 1000, 1000); ///< currently hard coded upper corner of the sim. domain
        BTree::ParallelTree tree_ = BTree::ParallelTree(loc_min_,loc_max_);
        ///< The parallel BTree (primarily for space charge calculation)

        std::vector<Core::Vector>  newPos_;  ///< new position (after time step) for particles
        std::vector<Core::Vector>  a_t_;     ///< last time step acceleration for particles
        std::vector<Core::Vector>  a_tdt_;   ///< new acceleration for particles

        void bearParticles_(double time);
        void initInternalState_();
    };
}


#endif /* BTree_parallelVerletIntegrator_hpp */
