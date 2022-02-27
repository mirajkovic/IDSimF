/***************************
 Ion Dynamics Simulation Framework (IDSimF)

 Copyright 2022 - Physical and Theoretical Chemistry /
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

 FMM3D_fmmSolver.hpp

 Coulombic Particle / Particle (space charge) solver based on FMM3D library

 ****************************/

#ifndef IDSIMF_FMM3D_FMMSOLVER_HPP
#define IDSIMF_FMM3D_FMMSOLVER_HPP

#include "SC_generic.hpp"
#include "Core_vector.hpp"
#include "Core_particle.hpp"
#include <list>
#include <vector>
#include <unordered_map>


namespace FMM3D{

    constexpr double NEGATIVE_ELECTRIC_CONSTANT = -1.0* Core::ELECTRIC_CONSTANT;

    struct particleListEntry{
        Core::Particle* particle;
        Core::Vector gradient;
        double potential;
    };

    using particlePtrList = std::list<particleListEntry>;
    /**
     * Abstract base class for Barnes-Hut Tree nodes
     */
    class FMMSolver : public SpaceCharge::FieldCalculator {

    public:
        FMMSolver();

        void insertParticle(Core::Particle &particle, std::size_t ext_index);
        void removeParticle(std::size_t ext_index);
        [[nodiscard]] std::size_t getNumberOfParticles() const;

        [[nodiscard]] Core::Vector getEFieldFromSpaceCharge(Core::Particle& particle) override;
        void computeChargeDistribution();

    private:
        std::unique_ptr<particlePtrList> iVec_; ///< a linked particle list, stores the particles in a linear order
        std::unique_ptr<std::unordered_map<std::size_t, particlePtrList::const_iterator>> iMap_; ///< a map between the ion indices (keys used by SIMION) and the pointers into the internal particle list
        std::unique_ptr<std::unordered_map<Core::Particle*, particlePtrList::const_iterator>> pMap_; ///< a map between the particle pointers and the pointers into the internal particle list
        //std::unique_ptr<std::vector<double>> gradients_; ///<
        //std::unique_ptr<std::vector<double>> potentials_; ///<
    };
}
#endif //IDSIMF_FMM3D_FMMSOLVER_HPP
