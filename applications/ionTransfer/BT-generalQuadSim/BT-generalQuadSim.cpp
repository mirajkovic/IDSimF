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
 BT-generalQuadSim.cpp

 Ion trajectory simulation (including space charge and hard sphere collisions) in an
 quadrupole including gas flow and non ideal geometry given by potential arrays

 ****************************/

#include "Core_randomGenerators.hpp"
#include "BTree_particle.hpp"
#include "BTree_tree.hpp"
#include "PSim_util.hpp"
#include "PSim_constants.hpp"
#include "PSim_trajectoryExplorerJSONwriter.hpp"
#include "PSim_interpolatedField.hpp"
#include "PSim_boxStartZone.hpp"
#include "PSim_verletIntegrator.hpp"
#include "CollisionModel_HardSphere.hpp"
#include "appUtils_simulationConfiguration.hpp"
#include "appUtils_logging.hpp"
#include "appUtils_stopwatch.hpp"
#include "appUtils_signalHandler.hpp"
#include <iostream>
#include <vector>

// some constants:
double freq_rf = 1.0e6; //Hz, RF Frequency
double omega_rf = freq_rf * M_PI *2.0;
const double rho_per_pa = 2.504e20; //(particles / m^3) / Pa

int main(int argc, const char * argv[]) {

    try {
        // read configuration file ======================================================================
        if (argc<=2) {
            std::cout << "Run abort: No run configuration or project name given." << std::endl;
            return EXIT_FAILURE;
        }

        std::string projectName = argv[2];
        std::cout << projectName << std::endl;
        auto logger = AppUtils::createLogger(projectName+".log");

        std::string confFileName = argv[1];
        AppUtils::SimulationConfiguration simConf(confFileName, logger);
        std::string confBasePath = simConf.confBasePath();



        // read basic simulation parameters =============================================================
        int timeSteps = simConf.intParameter("sim_time_steps");
        int trajectoryWriteInterval = simConf.intParameter("trajectory_write_interval");
        double dt = simConf.doubleParameter("dt");

        // read interpolated fields ======================
        std::unique_ptr<ParticleSimulation::InterpolatedField> rhoField = simConf.readInterpolatedField(
                "rho_field_file");
        std::unique_ptr<ParticleSimulation::InterpolatedField> flowField = simConf.readInterpolatedField(
                "flow_field_file");
        std::unique_ptr<ParticleSimulation::InterpolatedField> electricFieldQuadRF = simConf.readInterpolatedField(
                "electric_field_rf_file");
        std::unique_ptr<ParticleSimulation::InterpolatedField> electricFieldQuadEntrance = simConf.readInterpolatedField(
                "electric_field_entrance_file");

        // read physical and geometrical simulation parameters
        int collisionMode = simConf.intParameter("collision_mode");
        double spaceChargeFactor = simConf.doubleParameter("space_charge_factor");
        double collisionGasMassAmu = simConf.doubleParameter("collision_gas_mass_amu");
        double collisionGasDiameterM = simConf.doubleParameter("collision_gas_diameter_angstrom")*1e-10;
        double backgroundTemperture = simConf.doubleParameter("background_temperature");

        double V_rf = simConf.doubleParameter("V_rf");//600; //volts, RF voltage
        double V_entrance = simConf.doubleParameter("V_entrance");
        double P_factor = simConf.doubleParameter("P_factor");

        double entranceAperture = simConf.doubleParameter("entrance_aperture_mm")/1000.0;

        double qStartBoxCenter = simConf.doubleParameter("start_center_mm")/1000.0;
        double qStartBoxLength =
                simConf.doubleParameter("start_length_mm")/1000.0; //the start position on the q length (x) axis

        double maxQLength = simConf.doubleParameter("max_q_length_mm")/1000.0;
        double maxRadius = simConf.doubleParameter("max_r_mm")/1000.0;

        // read ion configuration ========================
        std::vector<int> nIons = simConf.intVectorParameter("n_ions");
        std::vector<double> ionMasses = simConf.doubleVectorParameter("ion_masses");

        std::vector<std::unique_ptr<BTree::Particle>> particles;
        std::vector<BTree::Particle*> particlePtrs;

        //prepare file writers ==============================================================================
        auto jsonWriter = std::make_unique<ParticleSimulation::TrajectoryExplorerJSONwriter>
                (projectName+"_trajectories.json");
        jsonWriter->setScales(1000, 1e6);

        // prepare random generators:
        //todo: reimplement ion start zone with ion start zone class
        ParticleSimulation::BoxStartZone startZone(
                {qStartBoxLength, 2*entranceAperture, 2*entranceAperture},
                {qStartBoxCenter, 0.0, 0.0});

        //init ions:
        for (int i = 0; i<nIons.size(); i++) {
            int nParticles = nIons[i];
            double mass = ionMasses[i];
            auto ions = startZone.getRandomParticlesInStartZone(nParticles, 1.0);

            for (int j = 0; j<nParticles; j++) {
                ions[j]->setMassAMU(mass);
                particlePtrs.push_back(ions[j].get());
                particles.push_back(std::move(ions[j]));
            }
        }

        auto backgroundGasVelocityFunction = [&flowField](Core::Vector& location) {
            Core::Vector flowVelo = flowField->getInterpolatedVector(location.x(), location.y(), location.z(), 0);
            return flowVelo;
        };

        auto backgroundGasPressureFunction = [&rhoField, P_factor](Core::Vector& location) {
            double rho = rhoField->getInterpolatedScalar(location.x(), location.y(), location.z(), 0);
            double pressure_pa = rho/rho_per_pa*P_factor;
            return pressure_pa;
        };


        //init gas collision models:
        CollisionModel::HardSphereModel hsModel = CollisionModel::HardSphereModel(
                backgroundGasPressureFunction,
                backgroundGasVelocityFunction,
                backgroundTemperture,
                collisionGasMassAmu,
                collisionGasDiameterM
        );


        // define functions for the trajectory integration ==================================================
        auto accelerationFunction = [V_rf, V_entrance, spaceChargeFactor, collisionMode, &electricFieldQuadRF, &electricFieldQuadEntrance](
                BTree::Particle* particle, int particleIndex, BTree::Tree& tree, double time, int timestep) {
            //x is the long quad axis
            Core::Vector pos = particle->getLocation();
            double particleCharge = particle->getCharge();

            try {
                Core::Vector E =
                        (electricFieldQuadRF->getInterpolatedVector(pos.x(), pos.y(), pos.z(), 0)*cos(omega_rf*time)
                                *V_rf)+
                                (electricFieldQuadEntrance->getInterpolatedVector(pos.x(), pos.y(), pos.z(), 0)
                                        *V_entrance);

                Core::Vector spaceChargeForce = tree.computeEFieldFromTree(*particle)*spaceChargeFactor;
                Core::Vector result = (E+spaceChargeForce)*particleCharge/particle->getMass();
                return (result);
            }
            catch (const std::invalid_argument& exception) { //is thrown if particle is not in domain of interpolated fields
                particle->setInvalid(true);
                return (Core::Vector(0.0, 0.0, 0.0));
            }
        };

        ParticleSimulation::partAttribTransformFctType additionalParameterTransformFct =
                [&backgroundGasPressureFunction](BTree::Particle* particle) -> std::vector<double> {
                    double pressure_pa = backgroundGasPressureFunction(particle->getLocation());
                    std::vector<double> result = {
                            particle->getVelocity().x(),
                            particle->getVelocity().y(),
                            particle->getVelocity().z(),
                            pressure_pa
                    };
                    return result;
                };

        auto timestepWriteFunction = [trajectoryWriteInterval, &additionalParameterTransformFct, &jsonWriter, &logger](
                std::vector<BTree::Particle*>& particles, BTree::Tree& tree, double time, int timestep,
                bool lastTimestep) {
            if (timestep%trajectoryWriteInterval==0) {
                logger->info("ts:{} time:{:.2e}", timestep, time);
                char buffer[100];
                int cx;
                cx = snprintf(buffer, 100, "%06d", timestep);
                jsonWriter->writeTimestep(particles, additionalParameterTransformFct, time, false);
            }
            if (lastTimestep) {
                jsonWriter->writeTimestep(particles, additionalParameterTransformFct, time, true);
                jsonWriter->writeSplatTimes(particles);
                jsonWriter->writeIonMasses(particles);
                logger->info("finished ts:{} time:{:.2e}", timestep, time);
            }
        };

        auto otherActionsFunction = [maxQLength, maxRadius, &startZone](Core::Vector& newPartPos,
                                                                        BTree::Particle* particle, int particleIndex,
                                                                        BTree::Tree& tree, double time, int timestep) {

            double r_pos = std::sqrt(newPartPos.y()*newPartPos.y()+newPartPos.z()*newPartPos.z());

            if (newPartPos.x()>maxQLength) {
                newPartPos.z(0.0);
            }

            if (r_pos>maxRadius || newPartPos.x()>maxQLength || particle->isInvalid()) {
                newPartPos = startZone.getRandomParticlePosition();
                particle->setInvalid(false);
            }
        };

        // simulate ===============================================================================================
        AppUtils::Stopwatch stopWatch;
        stopWatch.start();
        ParticleSimulation::VerletIntegrator verletIntegrator(
                particlePtrs,
                accelerationFunction, timestepWriteFunction, otherActionsFunction, ParticleSimulation::noFunction,
                &hsModel);
        AppUtils::SignalHandler::setReceiver(verletIntegrator);
        verletIntegrator.run(timeSteps, dt);

        stopWatch.stop();
        logger->info("CPU time: {} s", stopWatch.elapsedSecondsCPU());
        logger->info("Finished in {} seconds (wall clock time)", stopWatch.elapsedSecondsWall());
    }
    catch(const std::invalid_argument& ia){
        std::cout << ia.what() << std::endl;
        return EXIT_FAILURE;
    }
}