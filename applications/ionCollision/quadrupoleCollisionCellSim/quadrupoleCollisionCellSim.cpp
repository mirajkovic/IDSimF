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
 quadrupoleCollisionCellSim.cpp

 Ion trajectory simulation (including space charge and hard sphere collisions) in an
 quadrupole collision cell with hard sphere collision dynamics

 ****************************/

#include "Core_randomGenerators.hpp"
#include "Core_particle.hpp"
#include "PSim_util.hpp"
#include "FileIO_trajectoryHDF5Writer.hpp"
#include "PSim_particleStartSplatTracker.hpp"
#include "Integration_parallelVerletIntegrator.hpp"
#include "CollisionModel_HardSphere.hpp"
#include "appUtils_simulationConfiguration.hpp"
#include "appUtils_inputFileUtilities.hpp"
#include "appUtils_ionDefinitionReading.hpp"
#include "appUtils_logging.hpp"
#include "appUtils_stopwatch.hpp"
#include "appUtils_signalHandler.hpp"
#include "appUtils_commandlineParser.hpp"
#include <iostream>
#include <vector>

/**
 * Mode of ion termination:
 * TERMINATE = ions are terminated / splatted at electrodes / domain edges
 * RESTART = ions are restarted in their start zone
 */
enum IonTerminationMode{
    TERMINATE, RESTART
};

enum IonDataRecordMode{
    FULL, SIMPLE
};

// constants:
// const double rho_per_pa = 2.504e20; //(particles / m^3) / Pa

int main(int argc, const char * argv[]) {

    try {
        // parse commandline / create conf and logger ===================================================
        AppUtils::CommandlineParser cmdLineParser(argc, argv, "BT-quadrupoleCollisionCellSim",
                "Simulation of a quadrupolar collision cell", true);
        std::string simResultBasename = cmdLineParser.resultName();
        AppUtils::logger_ptr logger = cmdLineParser.logger();
        AppUtils::simConf_ptr simConf = cmdLineParser.simulationConfiguration();
        
        // read basic simulation parameters =============================================================
        unsigned int timeSteps = simConf->unsignedIntParameter("sim_time_steps");
        unsigned int trajectoryWriteInterval = simConf->unsignedIntParameter("trajectory_write_interval");
        double dt = simConf->doubleParameter("dt");

        // read physical and geometrical simulation parameters
        double spaceChargeFactor = simConf->doubleParameter("space_charge_factor");
        double collisionGasMass_amu = simConf->doubleParameter("collision_gas_mass_amu");
        double collisionGasDiameter_m = simConf->doubleParameter("collision_gas_diameter_angstrom")*1e-10;
        double backgroundGasTemperature_K = simConf->doubleParameter("background_gas_temperature_K");
        double backgroundGasPressure_pa = simConf->doubleParameter("background_gas_pressure_Pa");

        double V_rf = simConf->doubleParameter("V_rf"); //volts, RF voltage
        double freq_rf = simConf->doubleParameter("frequency_rf"); //Hz, RF Frequency
        double omega_rf = freq_rf*M_PI*2.0;


        //read potential arrays and potential array configuration =================================================
        // Note that fast adjust PAs are expected here
        std::vector<std::string> potentialArraysNames = simConf->stringVectorParameter("potential_arrays");
        double potentialArrayScale = simConf->doubleParameter("potential_array_scale");
        std::vector<std::unique_ptr<ParticleSimulation::SimionPotentialArray>> potentialArrays =
                AppUtils::readPotentialArrayFiles(potentialArraysNames, simConf->confBasePath(), potentialArrayScale,
                        true);

        //scaling factor of 0.1 because SIMION uses a value of 10000 in  Fast Adjust PAs and  mm to m is 1000 = 0.1
        std::vector<double> potentialsDc = simConf->doubleVectorParameter("dc_potentials");
        std::vector<double> potentialFactorsRf = simConf->doubleVectorParameter("rf_potential_factors");

        // defining simulation domain box (used for ion termination):
        std::array<std::array<double, 2>, 3> simulationDomainBoundaries;
        if (simConf->isParameter("simulation_domain_boundaries")) {
            // get manual simulation domain boundaries from config file
            simulationDomainBoundaries = simConf->double3dBox("simulation_domain_boundaries");
        }
        else {
            // TODO: use minimal Potential Array bounds as simulation domain
            throw std::invalid_argument("missing configuration value: simulation_domain_boundaries");
        }

        // Read ion termination mode configuration from simulation config
        std::string ionTerminationMode_str = simConf->stringParameter("termination_mode");
        IonTerminationMode ionTerminationMode;
        if (ionTerminationMode_str=="terminate") {
            ionTerminationMode = TERMINATE;
        }
        else if (ionTerminationMode_str=="restart") {
            if (AppUtils::isIonCloudDefinitionPresent(*simConf)) {
                throw std::invalid_argument("Ion restart mode is not possible with ion cloud file");
            }
            ionTerminationMode = RESTART;
        }
        else {
            throw std::invalid_argument("Invalid ion termination mode");
        }


        // Read ion data record mode configuration from simulation config
        std::string ionRecordMode_str = simConf->stringParameter("record_mode");
        IonDataRecordMode ionRecordMode;
        if (ionRecordMode_str=="full") {
            ionRecordMode = FULL;
        }
        else if (ionRecordMode_str=="simple") {
            ionRecordMode = SIMPLE;
        }
        else {
            throw std::invalid_argument("Invalid ion record mode");
        }

        //Read ion configuration and initialize ions:
        std::vector<std::unique_ptr<Core::Particle>> particles;
        std::vector<Core::Particle*> particlePtrs;

        AppUtils::readIonDefinition(particles, particlePtrs, *simConf);

        //init gas collision models:
        CollisionModel::HardSphereModel hsModel = CollisionModel::HardSphereModel(
                backgroundGasPressure_pa, backgroundGasTemperature_K,
                collisionGasMass_amu, collisionGasDiameter_m);

        // define functions for the trajectory integration ==================================================
        auto accelerationFunction =
                [spaceChargeFactor, omega_rf, V_rf, ionRecordMode, &potentialArrays, &potentialsDc, &potentialFactorsRf](
                        Core::Particle* particle, int /*particleIndex*/, SpaceCharge::FieldCalculator& scFieldCalculator, double time,
                        int /*timestep*/) -> Core::Vector {

                    Core::Vector pos = particle->getLocation();
                    double particleCharge = particle->getCharge();

                    double V_t = cos(omega_rf*time)*V_rf;

                    Core::Vector eField(0, 0, 0);
                    for (size_t i = 0; i<potentialArrays.size(); i++) {

                        Core::Vector paFieldRaw = potentialArrays[i]->getField(pos.x(), pos.y(), pos.z());
                        Core::Vector paEffectiveField =
                                paFieldRaw*potentialsDc.at(i)+
                                        paFieldRaw*potentialFactorsRf.at(i)*V_t;

                        eField = eField+paEffectiveField;
                    }
                    Core::Vector spaceChargeField(0, 0, 0);
                    if (spaceChargeFactor>0) {
                        spaceChargeField = scFieldCalculator.getEFieldFromSpaceCharge(*particle)*spaceChargeFactor;
                    }

                    if (ionRecordMode==FULL) {
                        particle->setFloatAttribute("field x", eField.x());
                        particle->setFloatAttribute("field y", eField.y());
                        particle->setFloatAttribute("field z", eField.z());
                        particle->setFloatAttribute("space charge x", spaceChargeField.x());
                        particle->setFloatAttribute("space charge y", spaceChargeField.y());
                        particle->setFloatAttribute("space charge z", spaceChargeField.z());
                    }

                    return ((eField+spaceChargeField)*particleCharge/particle->getMass());
                };

        FileIO::partAttribTransformFctType particleAttributeTransformFct_simple =
                [](Core::Particle* particle) -> std::vector<double> {
                    std::vector<double> result = {
                            particle->getVelocity().x(),
                            particle->getVelocity().y(),
                            particle->getVelocity().z()
                    };

                    return result;
                };

        FileIO::partAttribTransformFctType particleAttributeTransformFct_full =
                [](Core::Particle* particle) -> std::vector<double> {
                    std::vector<double> result = {
                            particle->getVelocity().x(),
                            particle->getVelocity().y(),
                            particle->getVelocity().z(),
                            particle->getFloatAttribute("field x"),
                            particle->getFloatAttribute("field y"),
                            particle->getFloatAttribute("field z"),
                            particle->getFloatAttribute("space charge x"),
                            particle->getFloatAttribute("space charge y"),
                            particle->getFloatAttribute("space charge z")
                    };

                    return result;
                };

        FileIO::partAttribTransformFctTypeInteger integerParticleAttributesTransformFct =
                [](Core::Particle* particle) -> std::vector<int> {
                    std::vector<int> result = {
                            particle->getIntegerAttribute("global index")
                    };
                    return result;
                };

        std::vector<std::string> integerParticleAttributesNames = {"global index"};

        //prepare file writers ==============================================================================
        auto hdf5Writer = std::make_unique<FileIO::TrajectoryHDF5Writer>(
                simResultBasename+"_trajectories.hd5");

        if (ionRecordMode==FULL) {
            std::vector<std::string> particleAttributeNames = {"velocity x", "velocity y", "velocity z",
                                                               "rf field x", "rf field y", "rf field z",
                                                               "space charge x", "space charge y", "space charge z"};
            hdf5Writer->setParticleAttributes(particleAttributeNames, particleAttributeTransformFct_full);
        }
        else {
            std::vector<std::string> particleAttributeNames = {"velocity x", "velocity y", "velocity z"};
            hdf5Writer->setParticleAttributes(particleAttributeNames, particleAttributeTransformFct_simple);
        }
        hdf5Writer->setParticleAttributes(integerParticleAttributesNames, integerParticleAttributesTransformFct);

        // Prepare ion start / stop tracker and ion start monitoring / ion termination functions
        ParticleSimulation::ParticleStartSplatTracker startSplatTracker;
        auto particleStartMonitoringFct = [&startSplatTracker](Core::Particle* particle, double time) {
            startSplatTracker.particleStart(particle, time);
        };

        std::size_t ionsInactive = 0;
        Integration::AbstractTimeIntegrator *integratorPtr;
        auto timestepWriteFunction =
                [trajectoryWriteInterval, ionRecordMode, &ionsInactive, &hdf5Writer, &startSplatTracker,
                 &integratorPtr, &logger](
                        std::vector<Core::Particle*>& particles,
                        double time, unsigned int timestep, bool lastTimestep) {

                    // check if simulation should be terminated (if all particles are terminated)
                    if (ionsInactive >= particles.size() && particles.size() > 0){
                        integratorPtr->setTerminationState();
                    }

                    if (timestep==0 && ionRecordMode==FULL) {
                        //if initial time step (integrator was not run) and full record mode:
                        //(if attributes are not initalized, attribute transform function will crash)
                        for (Core::Particle* particle: particles) {
                            particle->setFloatAttribute("field x", 0.0);
                            particle->setFloatAttribute("field y", 0.0);
                            particle->setFloatAttribute("field z", 0.0);
                            particle->setFloatAttribute("space charge x", 0.0);
                            particle->setFloatAttribute("space charge y", 0.0);
                            particle->setFloatAttribute("space charge z", 0.0);
                        }
                    }

                    if (lastTimestep) {
                        hdf5Writer->writeTimestep(particles, time);
                        hdf5Writer->writeStartSplatData(startSplatTracker);
                        hdf5Writer->finalizeTrajectory();
                        logger->info("finished ts:{} time:{:.2e}", timestep, time);
                    }
                    else if (timestep%trajectoryWriteInterval==0) {
                        logger->info("ts:{} time:{:.2e} ions existing:{} ions inactive:{}", timestep, time, particles.size(), ionsInactive);
                        hdf5Writer->writeTimestep(particles, time);
                    }
                };


        //define other actions according to ion termination mode:

        auto isIonTerminated = [&simulationDomainBoundaries, &potentialArrays](Core::Vector& newPartPos) -> bool {
            return (newPartPos.x()<=simulationDomainBoundaries[0][0] ||
                    newPartPos.x()>=simulationDomainBoundaries[0][1] ||
                    newPartPos.y()<=simulationDomainBoundaries[1][0] ||
                    newPartPos.y()>=simulationDomainBoundaries[1][1] ||
                    newPartPos.z()<=simulationDomainBoundaries[2][0] ||
                    newPartPos.z()>=simulationDomainBoundaries[2][1] ||
                    potentialArrays.at(0)->isElectrode(newPartPos.x(), newPartPos.y(), newPartPos.z()));
        };

        Integration::otherActionsFctType otherActionsFunction;

        if (ionTerminationMode==TERMINATE) {
            otherActionsFunction = [&isIonTerminated, &ionsInactive, &startSplatTracker](
                    Core::Vector& newPartPos, Core::Particle* particle,
                    int /*particleIndex*/,  double time, int /*timestep*/) {
                // if the ion is out of the boundary box or ion has hit an electrode: Terminate
                if (isIonTerminated(newPartPos)) {
                    startSplatTracker.particleSplat(particle, time);
                    particle->setActive(false);
                    particle->setSplatTime(time);
                    ionsInactive++;
                }
            };
        }
        else { //ion termination mode is RESTART
            std::shared_ptr<ParticleSimulation::ParticleStartZone> particleStartZone =
                    AppUtils::getStartZoneFromIonDefinition(*simConf);

            otherActionsFunction = [&isIonTerminated, pz = std::move(particleStartZone), &startSplatTracker](
                    Core::Vector& newPartPos, Core::Particle* particle,
                    int /*particleIndex*/,
                     double time, int /*timestep*/) {
                // if the ion is out of the boundary box or ion has hit an electrode: Restart in ion start zone
                if (isIonTerminated(newPartPos)) {
                    newPartPos = pz->getRandomParticlePosition();
                    startSplatTracker.particleRestart(particle, particle->getLocation(), newPartPos, time);
                }
            };
        }


        // simulate ===============================================================================================
        AppUtils::Stopwatch stopWatch;
        stopWatch.start();

        Integration::ParallelVerletIntegrator verletIntegrator(
                particlePtrs,
                accelerationFunction, timestepWriteFunction, otherActionsFunction, particleStartMonitoringFct,
                &hsModel);
        integratorPtr = &verletIntegrator;
        AppUtils::SignalHandler::setReceiver(verletIntegrator);
        verletIntegrator.run(timeSteps, dt);

        stopWatch.stop();

        logger->info("CPU time: {} s", stopWatch.elapsedSecondsCPU());
        logger->info("Finished in {} seconds (wall clock time)",stopWatch.elapsedSecondsWall());
    }
    catch(const ParticleSimulation::PotentialArrayException& pe)
    {
        std::cout << pe.what() << std::endl;
        return EXIT_FAILURE;
    }
    catch(AppUtils::TerminatedWhileCommandlineParsing& terminatedMessage){
        return terminatedMessage.returnCode();
    }
    catch(const std::invalid_argument& ia){
        std::cout << ia.what() << std::endl;
        return EXIT_FAILURE;
    }
}