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
 DMSSim.cpp

 Idealized plane electrode type differential ion mobility spectrometry (DMS) transport and chemistry simulation,
 including space chage and gas collision effects

 ****************************/

#include "Core_utils.hpp"
#include "Core_randomGenerators.hpp"
#include "RS_Simulation.hpp"
#include "RS_SimulationConfiguration.hpp"
#include "RS_ConfigFileParser.hpp"
#include "RS_ConcentrationFileWriter.hpp"
#include "PSim_util.hpp"
#include "PSim_constants.hpp"
#include "Integration_parallelVerletIntegrator.hpp"
#include "FileIO_trajectoryHDF5Writer.hpp"
#include "FileIO_scalar_writer.hpp"
#include "FileIO_MolecularStructureReader.hpp"
#include "CollisionModel_StatisticalDiffusion.hpp"
#include "CollisionModel_HardSphere.hpp"
#include "CollisionModel_MDInteractions.hpp"
#include "CollisionModel_SpatialFieldFunctions.hpp"
#include "appUtils_simulationConfiguration.hpp"
#include "appUtils_logging.hpp"
#include "appUtils_stopwatch.hpp"
#include "appUtils_signalHandler.hpp"
#include "appUtils_commandlineParser.hpp"
#include "dmsSim_dmsFields.hpp"
#include <iostream>
#include <cmath>

const std::string key_ChemicalIndex = "keyChemicalIndex";
enum FlowMode {UNIFORM_FLOW,PARABOLIC_FLOW};
enum BackgroundTemperatureMode {ISOTHERM,LINEAR_GRADIENT};
enum CollisionType {SDS, HS, MD, NO_COLLISION};

int main(int argc, const char * argv[]) {

    try {
        // open configuration, parse configuration file =========================================
        AppUtils::CommandlineParser cmdLineParser(argc, argv, "BT-RS-DMSSim", "DMS Simulation with trajectories and chemistry", true);
        std::string projectName = cmdLineParser.resultName();
        AppUtils::logger_ptr logger = cmdLineParser.logger();

        std::string confFileName = cmdLineParser.confFileName();
        AppUtils::simConf_ptr simConf = cmdLineParser.simulationConfiguration();

        // optionally setting random generator seed manually (for debugging / reproduction purposes):
        if (simConf->isParameter("random_seed")) {
            unsigned int randomSeed = simConf->unsignedIntParameter("random_seed");
            Core::globalRandomGeneratorPool->setSeedForElements(randomSeed);
        }


        std::vector<unsigned int> nParticles = simConf->unsignedIntVectorParameter("n_particles");
        unsigned int nSteps = simConf->unsignedIntParameter("sim_time_steps");
        unsigned int nStepsPerOscillation = simConf->unsignedIntParameter("sim_time_steps_per_sv_oscillation");
        int concentrationWriteInterval = simConf->intParameter("concentrations_write_interval");
        int trajectoryWriteInterval = simConf->intParameter("trajectory_write_interval");
        double spaceChargeFactor = simConf->doubleParameter("space_charge_factor");


        //geometric parameters:
        double startWidthX_m = simConf->doubleParameter("start_width_x_mm")/1000.0;
        double startWidthY_m = simConf->doubleParameter("start_width_y_mm")/1000.0;
        double startWidthZ_m = simConf->doubleParameter("start_width_z_mm")/1000.0;
        double electrodeDistance_m = simConf->doubleParameter("electrode_distance_mm")/1000.0;
        double electrodeLength_m = simConf->doubleParameter("electrode_length_mm")/1000.0;
        double electrodeHalfDistance_m = electrodeDistance_m/2.0;
        double electrodeHalfDistanceSquared_m = electrodeHalfDistance_m*electrodeHalfDistance_m;


        //background gas parameters:
        std::string collisionTypeStr = simConf->stringParameter("collision_model");
        CollisionType collisionType;
        if (collisionTypeStr=="SDS") {
            collisionType = SDS;
        }
        else if (collisionTypeStr=="HS"){
            collisionType = HS;
        }
        else if (collisionTypeStr=="MD"){
            collisionType = MD;
        }
        else if (collisionTypeStr=="none") {
            collisionType = NO_COLLISION;
        }
        else {
            throw std::invalid_argument("wrong configuration value: collision_model_type");
        }

        std::string flowModeStr = simConf->stringParameter("flow_mode");
        FlowMode flowMode;
        if (flowModeStr=="uniform") {
            flowMode = UNIFORM_FLOW;
        }
        else if (flowModeStr=="parabolic") {
            flowMode = PARABOLIC_FLOW;
        }
        else {
            throw std::invalid_argument("wrong configuration value: flow_mode");
        }

        //Define background temperature function for chemical reaction and collision model
        //BackgroundTemperatureMode backgroundTempMode;
        std::function<double(const Core::Vector&)> backgroundTemperatureFct;
        std::string backgroundTempStr = simConf->stringParameter("background_temperature_mode");
        if (backgroundTempStr=="isotherm") {
            //backgroundTempMode = ISOTHERM;
            double backgroundTemperature_K = simConf->doubleParameter("background_temperature_K");
            backgroundTemperatureFct = CollisionModel::getConstantDoubleFunction(backgroundTemperature_K);
        }
        else if (backgroundTempStr=="linear_gradient") {
            //backgroundTempMode = LINEAR_GRADIENT;
            double backgroundTemp_start = simConf->doubleParameter("background_temperature_start_K");
            double backgroundTemp_stop = simConf->doubleParameter("background_temperature_stop_K");
            double backgroundTemp_diff = backgroundTemp_stop-backgroundTemp_start;

            backgroundTemperatureFct =
                    [backgroundTemp_start,
                            backgroundTemp_stop,
                            backgroundTemp_diff,
                            electrodeLength_m](const Core::Vector& particleLocation) -> double {

                        if (particleLocation.x()>electrodeLength_m) {
                            return backgroundTemp_stop;
                        }
                        else {
                            return (backgroundTemp_diff/electrodeLength_m*particleLocation.x())+backgroundTemp_start;
                        }
                    };
        }
        else {
            throw std::invalid_argument("wrong configuration value: background_temperature_mode");
        }

        double backgroundPressure_Pa = simConf->doubleParameter("background_pressure_Pa");
        double gasVelocityX = simConf->doubleParameter("collision_gas_velocity_x_ms-1");
        double collisionGasMass_Amu = simConf->doubleParameter("collision_gas_mass_amu");
        double collisionGasDiameter_nm = simConf->doubleParameter("collision_gas_diameter_nm");


        //field parameters:
        CVMode cvMode = parseCVModeConfiguration(simConf);
        double meanZPos = 0.0; //variable used for automatic CV correction
        double cvRelaxationParameter = 0.0;
        if (cvMode == AUTO_CV || cvMode == MODULATED_AUTO_CV){
            cvRelaxationParameter = simConf->doubleParameter("cv_relaxation_parameter");
        }

        SVMode svMode = parseSVModeConfiguration(simConf);

        double fieldSVSetpoint_VPerM = simConf->doubleParameter("sv_Vmm-1") * 1000.0;
        double fieldCVSetpoint_VPerM = simConf->doubleParameter("cv_Vmm-1") * 1000.0;
        double fieldFrequency = simConf->doubleParameter("sv_frequency_s-1");
        double fieldWavePeriod = 1.0/fieldFrequency;

        double dt_s = fieldWavePeriod/nStepsPerOscillation;
        // ======================================================================================


        //read and prepare chemical configuration ===============================================
        RS::ConfigFileParser parser = RS::ConfigFileParser();
        std::string rsConfFileName = simConf->pathRelativeToConfFile(simConf->stringParameter("reaction_configuration"));
        RS::Simulation rsSim = RS::Simulation(parser.parseFile(rsConfFileName));
        RS::SimulationConfiguration* rsSimConf = rsSim.simulationConfiguration();

        //prepare a map for retrieval of the substance index:
        std::map<RS::Substance*, int> substanceIndices;
        std::vector<RS::Substance*> discreteSubstances = rsSimConf->getAllDiscreteSubstances();
        std::vector<double> ionMobility; // = simConf->doubleVectorParameter("ion_mobility");
        for (std::size_t i = 0; i<discreteSubstances.size(); ++i) {
            substanceIndices.insert(std::pair<RS::Substance*, int>(discreteSubstances[i], i));
            ionMobility.push_back(discreteSubstances[i]->lowFieldMobility());
        }

        //read molecular structure file
        std::unordered_map<std::string,  std::shared_ptr<CollisionModel::MolecularStructure>> molecularStructureCollection;
        if(collisionType==MD){
            std::string mdCollisionConfFile = simConf->pathRelativeToConfFile(simConf->stringParameter("md_configuration"));
            FileIO::MolecularStructureReader mdConfReader = FileIO::MolecularStructureReader();
            molecularStructureCollection = mdConfReader.readMolecularStructure(mdCollisionConfFile);
        }


        // prepare file writer  =================================================================
        RS::ConcentrationFileWriter resultFilewriter(projectName+"_conc.csv");

        std::vector<std::string> auxParamNames = {"chemical id"};
        auto additionalParamTFct = [](Core::Particle* particle) -> std::vector<int> {
            std::vector<int> result = {
                    particle->getIntegerAttribute(key_ChemicalIndex)
            };
            return result;
        };

        std::string hdf5Filename = projectName+"_trajectories.hd5";
        FileIO::TrajectoryHDF5Writer trajectoryWriter(hdf5Filename);
        trajectoryWriter.setParticleAttributes(auxParamNames, additionalParamTFct);

        unsigned int ionsInactive = 0;
        unsigned int nAllParticles = 0;
        for (const auto ni: nParticles) {
            nAllParticles += ni;
        }

        std::unique_ptr<FileIO::Scalar_writer> cvFieldWriter;
        if (cvMode==AUTO_CV) {
            cvFieldWriter = std::make_unique<FileIO::Scalar_writer>(projectName+"_cv.csv");
        }

        std::unique_ptr<FileIO::Scalar_writer> voltageWriter;
        voltageWriter = std::make_unique<FileIO::Scalar_writer>(projectName+"_voltages.csv");


        // init simulation  =====================================================================

        // create and add simulation particles:
        unsigned int nParticlesTotal = 0;
        std::vector<uniqueReactivePartPtr> particles;
        std::vector<Core::Particle*> particlesPtrs;
        std::vector<std::vector<double>> trajectoryAdditionalParams;

        Core::Vector initCorner(0, -startWidthY_m/2.0, -startWidthZ_m/2.0);
        Core::Vector initBoxSize(startWidthX_m, startWidthY_m, startWidthZ_m);

        for (std::size_t i = 0; i<nParticles.size(); i++) {
            RS::Substance* subst = rsSimConf->substance(i);
            int substIndex = substanceIndices.at(subst);
            std::vector<Core::Vector> initialPositions =
                    ParticleSimulation::util::getRandomPositionsInBox(nParticles[i], initCorner, initBoxSize);
            for (unsigned int k = 0; k<nParticles[i]; ++k) {
                uniqueReactivePartPtr particle = std::make_unique<RS::ReactiveParticle>(subst);

                particle->setLocation(initialPositions[k]);
                particle->setIntegerAttribute(key_ChemicalIndex, substIndex);
                particle->setIndex(nParticlesTotal);

                particlesPtrs.push_back(particle.get());
                rsSim.addParticle(particle.get(), nParticlesTotal);
                particles.push_back(std::move(particle));
                trajectoryAdditionalParams.push_back(std::vector<double>(1));
                nParticlesTotal++;
            }
        }

        resultFilewriter.initFile(rsSimConf);
        // ======================================================================================


        // define trajectory integration parameters / functions =================================
        SVFieldFctType SVFieldFct = createSVFieldFunction(svMode, fieldWavePeriod);
        CVFieldFctType CVFieldFct = createCVFieldFunction(cvMode, fieldWavePeriod, simConf);

        double totalFieldNow_VPerM = 0.0;

        auto accelerationFct =
                [&totalFieldNow_VPerM, spaceChargeFactor]
                        (Core::Particle* particle, int /*particleIndex*/, SpaceCharge::FieldCalculator& scFieldCalculator, double /*time*/, int /*timestep*/) {

                    double particleCharge = particle->getCharge();
                    Core::Vector fieldForce(0, 0, totalFieldNow_VPerM*particleCharge);

                    if (Core::isDoubleEqual(spaceChargeFactor, 0.0)) {
                        return (fieldForce/particle->getMass());
                    }
                    else {
                        Core::Vector spaceChargeForce =
                                scFieldCalculator.getEFieldFromSpaceCharge(*particle)*(particleCharge*spaceChargeFactor);
                        return ((fieldForce+spaceChargeForce)/particle->getMass());
                    }
                };


        auto timestepWriteFct =
                [&trajectoryWriter, &voltageWriter, trajectoryWriteInterval, &rsSim, &resultFilewriter, concentrationWriteInterval,
                 &totalFieldNow_VPerM, &logger]
                        (std::vector<Core::Particle*>& particles, double time, int timestep,
                         bool lastTimestep) {

                    if (timestep%concentrationWriteInterval==0) {
                        resultFilewriter.writeTimestep(rsSim);
                        voltageWriter->writeTimestep(totalFieldNow_VPerM, time);
                    }
                    if (lastTimestep) {
                        trajectoryWriter.writeTimestep(particles, time);
                        trajectoryWriter.writeSplatTimes(particles);
                        trajectoryWriter.finalizeTrajectory();
                        logger->info("finished ts:{} time:{:.2e}", timestep, time);
                    }
                    else if (timestep%trajectoryWriteInterval==0) {
                        logger->info("ts:{}  time:{:.2e}",
                                timestep, time);
                        rsSim.logConcentrations(logger);
                        trajectoryWriter.writeTimestep(particles, time);
                    }
                };

        auto otherActionsFct = [electrodeHalfDistance_m, electrodeLength_m, &ionsInactive](
                Core::Vector& newPartPos, Core::Particle* particle,
                int /*particleIndex*/,  double time, int /*timestep*/) {

            if (std::fabs(newPartPos.z())>=electrodeHalfDistance_m) {
                particle->setActive(false);
                particle->setSplatTime(time);
                ionsInactive++;
            }
            else if (newPartPos.x()>=electrodeLength_m) {
                particle->setActive(false);
                ionsInactive++;
            }
        };


        //define / gas interaction /  collision model:
        std::unique_ptr<CollisionModel::AbstractCollisionModel> collisionModelPtr;
        if (collisionType==SDS || collisionType==HS || collisionType==MD) {
            // prepare static pressure and temperature functions
            auto staticPressureFct = CollisionModel::getConstantDoubleFunction(backgroundPressure_Pa);

            std::function<Core::Vector(const Core::Vector&)> velocityFct;

            if (flowMode==UNIFORM_FLOW) {
                velocityFct =
                        [gasVelocityX](const Core::Vector& /*pos*/) {
                            return Core::Vector(gasVelocityX, 0.0, 0.0);
                        };
            }
            else if (flowMode==PARABOLIC_FLOW) {
                velocityFct =
                        [gasVelocityX, electrodeHalfDistanceSquared_m](const Core::Vector& pos) {
                            //parabolic profile is vX = 2 * Vavg * (1 - r^2 / R^2) with the radius / electrode distance R
                            double xVelo = gasVelocityX*2.0*(1-pos.z()*pos.z()/electrodeHalfDistanceSquared_m);
                            return Core::Vector(xVelo, 0.0, 0.0);
                        };
            }

            if (collisionType==SDS){
                std::unique_ptr<CollisionModel::StatisticalDiffusionModel> collisionModel =
                        std::make_unique<CollisionModel::StatisticalDiffusionModel>(
                                staticPressureFct,
                                backgroundTemperatureFct,
                                velocityFct,
                                collisionGasMass_Amu,
                                collisionGasDiameter_nm*1e-9);

                for (const auto& particle: particlesPtrs) {
                    particle->setDiameter(
                            CollisionModel::util::estimateCollisionDiameterFromMass(
                                    particle->getMass()/Core::AMU_TO_KG
                                    )*1e-9);
                    collisionModel->setSTPParameters(*particle);
                }
                collisionModelPtr = std::move(collisionModel);
            }
            else if (collisionType==HS){
                std::unique_ptr<CollisionModel::HardSphereModel> collisionModel =
                        std::make_unique<CollisionModel::HardSphereModel>(
                                staticPressureFct,
                                velocityFct,
                                backgroundTemperatureFct,
                                collisionGasMass_Amu,
                                collisionGasDiameter_nm*1e-9,
                                nullptr);
                collisionModelPtr = std::move(collisionModel);
            }
            else if (collisionType==MD){

                // collect additional config file parameters for MD model:
                double collisionGasPolarizability_m3 = simConf->doubleParameter("collision_gas_polarizability_m3");
                std::string collisionGasIdentifier = simConf->stringParameter("collision_gas_identifier");
                std::vector<std::string> particleIdentifiers = simConf->stringVectorParameter("particle_identifier");
                double subIntegratorIntegrationTime_s = simConf->doubleParameter("sub_integrator_integration_time_s");
                double subIntegratorStepSize_s = simConf->doubleParameter("sub_integrator_step_size_s");
                double collisionRadiusScaling = simConf->doubleParameter("collision_radius_scaling");
                double angleThetaScaling = simConf->doubleParameter("angle_theta_scaling");
                double spawnRadius_m = simConf->doubleParameter("spawn_radius_m");

                //construct MD model:
                std::unique_ptr<CollisionModel::MDInteractionsModel> collisionModel =
                        std::make_unique<CollisionModel::MDInteractionsModel>(
                            staticPressureFct,
                            velocityFct,
                            backgroundTemperatureFct,
                            collisionGasMass_Amu,
                            collisionGasDiameter_nm*1e-9,
                            collisionGasPolarizability_m3,
                            collisionGasIdentifier,
                            subIntegratorIntegrationTime_s,
                            subIntegratorStepSize_s,
                            collisionRadiusScaling,
                            angleThetaScaling,
                            spawnRadius_m,
                            molecularStructureCollection);

                // Set trajectory writing options:
                bool saveTrajectory = simConf->boolParameter("save_trajectory");

                if (saveTrajectory){
                    int saveTrajectoryStartTimeStep = simConf->intParameter("trajectory_start_time_step");
                    double trajectoryDistance_m = simConf->doubleParameter("trajectory_distance_m");
                    collisionModel->setTrajectoryWriter(projectName+"_md_trajectories.txt",
                            trajectoryDistance_m, saveTrajectoryStartTimeStep);
                }


                // Init particles with MD parameters:
                unsigned int particleIndex = 0;
                for (std::size_t i = 0; i<nParticles.size(); i++) {
                    for (unsigned int k = 0; k<nParticles[i]; k++) {
                        Core::Particle* particle = particlesPtrs[particleIndex];
                        particle->setMolecularStructure(molecularStructureCollection.at(particleIdentifiers[i]));
                        particle->setDiameter(particle->getMolecularStructure()->getDiameter());
                        particleIndex++;
                    }
                }

                collisionModelPtr = std::move(collisionModel);
            }
        }
        else if (collisionType==NO_COLLISION) {
            collisionModelPtr = nullptr;
        }

        //define reaction simulation functions:
        auto particlesHasReactedFct = [&collisionModelPtr, &substanceIndices](RS::ReactiveParticle* particle){
            //we had an reaction event: Count it (access to total counted value has to be synchronized)
            if (collisionModelPtr != nullptr) {
                collisionModelPtr->initializeModelParticleParameters(*particle);
            }
            int substIndex = substanceIndices.at(particle->getSpecies());
            particle->setIntegerAttribute(key_ChemicalIndex, substIndex);
        };

        auto reactionConditionsFct = [&totalFieldNow_VPerM, &backgroundTemperatureFct, backgroundPressure_Pa]
                (RS::ReactiveParticle* particle, double /*time*/)->RS::ReactionConditions{
            RS::ReactionConditions reactionConditions = RS::ReactionConditions();

            reactionConditions.temperature = backgroundTemperatureFct(particle->getLocation());
            reactionConditions.electricField = totalFieldNow_VPerM;
            reactionConditions.pressure = backgroundPressure_Pa;
            return reactionConditions;
        };

        //init trajectory simulation object:
        Integration::ParallelVerletIntegrator verletIntegrator(
                particlesPtrs,
                accelerationFct, timestepWriteFct, otherActionsFct, ParticleSimulation::noFunction,
                collisionModelPtr.get());
        // ======================================================================================


        // simulate   ===========================================================================
        AppUtils::SignalHandler::setReceiver(verletIntegrator);
        AppUtils::Stopwatch stopWatch;
        stopWatch.start();

        for (unsigned int step = 0; step<nSteps; step++) {
            double cvFieldNow_VPerM = CVFieldFct(fieldCVSetpoint_VPerM, rsSim.simulationTime());
            double svFieldNow_VPerM = SVFieldFct(fieldSVSetpoint_VPerM, rsSim.simulationTime());
            totalFieldNow_VPerM = svFieldNow_VPerM + cvFieldNow_VPerM;
            rsSim.performTimestep(reactionConditionsFct, dt_s, particlesHasReactedFct);
            rsSim.advanceTimestep(dt_s);
            verletIntegrator.runSingleStep(dt_s);

            //autocorrect compensation voltage, to minimize z drift (once for every single SV oscillation):
            if (cvMode==AUTO_CV && step%nStepsPerOscillation==0) {
                //calculate current mean z-position:
                double buf = 0.0;
                for (unsigned int i = 0; i<nParticlesTotal; i++) {
                    buf += particles[i]->getLocation().z();
                }
                double currentMeanZPos = buf/nParticlesTotal;

                //update cv value:
                double diffMeanZPos = meanZPos-currentMeanZPos;
                fieldCVSetpoint_VPerM = fieldCVSetpoint_VPerM+diffMeanZPos*cvRelaxationParameter;
                cvFieldWriter->writeTimestep(std::vector<double>{fieldCVSetpoint_VPerM, currentMeanZPos},
                        rsSim.simulationTime());
                meanZPos = currentMeanZPos;
                logger->info("CV corrected ts:{} time:{:.2e} new CV:{} diffMeanPos:{}", step, rsSim.simulationTime(), fieldCVSetpoint_VPerM, diffMeanZPos);
            }

            //terminate simulation loops if all particles are terminated or termination of the integrator was requested
            //from somewhere (e.g. signal from outside)
            if (ionsInactive>=nAllParticles ||
                    verletIntegrator.runState()==Integration::AbstractTimeIntegrator::IN_TERMINATION)
            {
                break;
            }
        }
        verletIntegrator.finalizeSimulation();
        resultFilewriter.closeFile();
        stopWatch.stop();

        logger->info("total reaction events: {} ill events: {}", rsSim.totalReactionEvents(), rsSim.illEvents());
        logger->info("ill fraction: {}", rsSim.illEvents()/(double) rsSim.totalReactionEvents());
        logger->info("CPU time: {} s", stopWatch.elapsedSecondsCPU());
        logger->info("Finished in {} seconds (wall clock time)", stopWatch.elapsedSecondsWall());
        // ======================================================================================

        return EXIT_SUCCESS;
    }
    catch(AppUtils::TerminatedWhileCommandlineParsing& terminatedMessage){
        return terminatedMessage.returnCode();
    }
    catch(const std::invalid_argument& ia){
        std::cout << ia.what() << std::endl;
        return EXIT_FAILURE;
    }
}
