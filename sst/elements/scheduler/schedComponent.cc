// Copyright 2009-2014 Sandia Corporation. Under the terms
// of Contract DE-AC04-94AL85000 with Sandia Corporation, the U.S.
// Government retains certain rights in this software.
// 
// Copyright (c) 2009-2014, Sandia Corporation
// All rights reserved.
// 
// This file is part of the SST software package. For license
// information, see the LICENSE file in the top level directory of the
// distribution.

#include "sst_config.h"
#include "sst/core/serialization.h"
#include "sst/core/rng/mersenne.h"
#include "schedComponent.h" 

#include <assert.h>
#include <boost/algorithm/string.hpp>
#include <boost/filesystem.hpp>
#include <boost/thread.hpp>
#include <cstdlib>
#include <cstring>
#include <fstream>

//#include <iostream> //debug

#include <sst/core/debug.h>
#include <sst/core/element.h>
#include <sst/core/params.h>

#include "Allocator.h"
#include "AllocInfo.h"
#include "Factory.h"
#include "FST.h"
#include "InputParser.h"
#include "Job.h"
#include "Machine.h"
#include "MachineMesh.h"
#include "MeshAllocInfo.h"
#include "misc.h"
#include "Scheduler.h"
#include "Statistics.h"
#include "TaskMapper.h"

#include "events/ArrivalEvent.h"
#include "events/CompletionEvent.h"
#include "events/CommunicationEvent.h"
#include "events/FaultEvent.h"
#include "events/FinalTimeEvent.h"
#include "events/JobKillEvent.h"
#include "events/JobStartEvent.h"

using namespace std;
using namespace SST;
using namespace SST::Scheduler;

extern int yumyumFaultRand48Seed;
extern int yumyumErrorLogRand48Seed;
extern int yumyumErrorLatencyRand48Seed;
extern int yumyumErrorCorrectionRand48Seed;
extern int yumyumJobKillRand48Seed;

//necessary for sorting a list of pointers to completion events
//can't overload < because that does not work with pointers
bool compareCEPointers(CompletionEvent* ev1, CompletionEvent* ev2) 
{
    return ev1 -> jobNum < ev2 -> jobNum; 
}

Machine* schedComponent::getMachine() 
{
    return machine;
}

schedComponent::~schedComponent()
{
    delete stats;
    delete scheduler;
    delete rng;
    if (FSTtype > 0) delete calcFST;
}

int readSeed( Params & params, std::string paramName ){
	if( params.find( paramName ) != params.end() ){
		return atoi( params[ paramName ].c_str() );
	}else if( params.find( "seed" ) != params.end() ){
		return atoi( params[ "seed" ].c_str() );
	}else{
		return time( NULL );
	}
}


schedComponent::schedComponent(ComponentId_t id, Params& params) :
    Component(id) 
{
    lastfinaltime = ~0;

    if (params.find("traceName") == params.end()) {
        _abort(event_test,"couldn't find trace name\n");
    }


    //set up output
    schedout.init("", 8, 0, Output::STDOUT);

    // tell the simulator not to end without us
    registerThis();

    // get the PRNG seeds we'll use

    yumyumFaultRand48Seed = readSeed( params, std::string( "faultSeed" ) );
    yumyumErrorLogRand48Seed = readSeed( params, std::string( "errorLogSeed" ) );
    yumyumErrorLatencyRand48Seed = readSeed( params, std::string( "errorLatencySeed" ) );
    yumyumErrorCorrectionRand48Seed = readSeed( params, std::string( "errorCorrectionSeed" ) );
    yumyumJobKillRand48Seed = readSeed( params, std::string( "jobKillSeed" ) );
    // get running time RNG seed
    string runningTimeSeed = "none";
    if( params.find("runningTimeSeed") != params.end() ){
        runningTimeSeed = params["runningTimeSeed"];
    }
    if( runningTimeSeed.compare("none") != 0 ){
         rng = new SST::RNG::MersenneRNG( atoi(runningTimeSeed.c_str()) );
    } else {
        rng = new SST::RNG::MersenneRNG();
    }

    //set up the all-important self-link
    selfLink = configureSelfLink("linkToSelf", new Event::Handler<schedComponent>(this, &schedComponent::handleJobArrivalEvent));
    selfLink->setDefaultTimeBase(registerTimeBase(SCHEDULER_TIME_BASE));

    // configure links
    bool done = 0;
    int nodeCount = 0;
    // connect links till we can't find any
    schedout.output("Scheduler looking for link...");
    while (!done) {
        char name[50];
        snprintf(name, 50, "nodeLink%d", nodeCount);
        schedout.output(" %s", name);
        SST::Link *l = configureLink(name, SCHEDULER_TIME_BASE, new Event::Handler<schedComponent,int>(this, &schedComponent::handleCompletionEvent, nodeCount));
        if (l) {

            nodes.push_back(l);
            nodeCount++;
        } else {
            schedout.output("(no %s)", name);
            done = 1;
        }
    }
    schedout.output("\n");

    Factory factory;
    machine = factory.getMachine(params, nodes.size(), this);
    scheduler = factory.getScheduler(params, nodes.size());
    theAllocator = factory.getAllocator(params, machine);
    theTaskMapper = factory.getTaskMapper(params, machine);
    FSTtype = factory.getFST(params);
    timePerDistance = factory.getTimePerDistance(params);
    string trace = params["traceName"].c_str();
    if (FSTtype > 0) {
        calcFST = new FST(FSTtype);  //must call calcFST -> setup() once we know the number of jobs (in other words, in setup())
    } else {
        calcFST = NULL;
    }

    if (NULL == dynamic_cast<MachineMesh*>(machine)) {
        char timestring[] = "time";
        stats = new Statistics(machine, scheduler, theAllocator, trace, timestring, false, calcFST);
    } else {
        //the alloc output only works on a mesh because it calculates L1 distances
        char timestring[] = "time,alloc";
        stats = new Statistics(machine, scheduler, theAllocator, trace, timestring, false, calcFST);
    }

    useYumYumSimulationKill = params.find("useYumYumSimulationKill") != params.end();
    YumYumSimulationKillFlag = false;

    if (params.find("YumYumPollWait") != params.end()) {
        YumYumPollWait = atoi(params.find("YumYumPollWait")->second.c_str() );
    }else{
        YumYumPollWait = 250;
    }

    useYumYumTraceFormat = params.find("useYumYumTraceFormat") != params.end();
    printYumYumJobLog = params.find("printYumYumJobLog") != params.end();
    printJobLog = params.find("printJobLog") != params.end();

    jobParser = new JobParser(machine, params, &useYumYumSimulationKill, &YumYumSimulationKillFlag);

    schedout.output("\nScheduler Detects %d nodes\n", (int)nodes.size());

    machine -> reset();
    scheduler -> reset();

    jobLogFileName = params["jobLogFileName"];
}


void schedComponent::setup()
{

    for( vector<SST::Link *>::iterator nodeIter = nodes.begin(); nodeIter != nodes.end(); nodeIter ++ ){
        // ask the newly-connected node for its ID
        SST::Event * getID = new CommunicationEvent( RETRIEVE_ID );
        (*nodeIter)->send( getID );
    }

    // done setting up the links, now read the job list

    jobs = jobParser -> parseJobs(getCurrentSimTime());
    
    for(int i = 0; i < (int) jobs.size(); i++)
    {
        ArrivalEvent* ae = new ArrivalEvent(jobs[i].getArrivalTime(), i);
        if (useYumYumTraceFormat) {
            selfLink -> send(0, ae); 
        } else {
            selfLink -> send(jobs[i].getArrivalTime(), ae); 
        }
    }
        
    if( useYumYumTraceFormat ){
        char* inputDir = getenv("SIMINPUT");
        string jobListFileName;
        if (inputDir != NULL) {
            jobListFileName = inputDir + trace;
        } else {
            jobListFileName = trace;
        }
        // in case there were no jobs in the joblist
        if( jobs.empty() ){
            unregisterYourself();
        }
        CommunicationEvent * CommEvent = new CommunicationEvent( START_FILE_WATCH );
        CommEvent->payload = & jobListFileName;
        selfLink->send( CommEvent ); 
    }

    if (FSTtype > 0){
        calcFST -> setup(jobs.size());
    }
}


schedComponent::schedComponent() : Component(-1)
{
    // for serialization only
}


void schedComponent::registerThis()
{
    registerAsPrimaryComponent();
    primaryComponentDoNotEndSim();
    registrationStatus = true;
}


void schedComponent::unregisterThis()
{
    primaryComponentOKToEndSim();
    registrationStatus = false;
}


bool schedComponent::isRegistered()
{
    return registrationStatus;
}


// If we aren't using the Yum Yum functionality, this will just unregister and return;
// Otherwise, this will block, checking if there are new jobs at intervals defined by YumYumPollWait, or if the YumYumSimulationKillFlag has been set.
void schedComponent::unregisterYourself()
{
    if (useYumYumSimulationKill) {
        while( YumYumSimulationKillFlag != true && jobs.empty() ){
            boost::this_thread::sleep( boost::posix_time::milliseconds( YumYumPollWait ) );
            if (jobParser -> checkJobFile()) {
                jobs = jobParser -> parseJobs(getCurrentSimTime());
                if (!jobs.empty()) {
                    break;
                }
            }
            if (YumYumSimulationKillFlag) {
                unregisterThis();
            }
        }
    } else {
        unregisterThis();
    }

    if (!isRegistered() && jobLog.is_open()) {
        jobLog.close();
    }
}


void schedComponent::startNextJob()
{
    CommunicationEvent * CommEvent = new CommunicationEvent (START_NEXT_JOB);
    if (useYumYumTraceFormat) {
        selfLink -> send(1, CommEvent); 
    } else {
        selfLink -> send(0, CommEvent); 
    }
}


// incoming events are scanned and deleted. ev is the returned event,
// node is the node it came from.
void schedComponent::handleCompletionEvent(Event *ev, int node) 
{
    if (dynamic_cast<CommunicationEvent *>(ev)) {
        CommunicationEvent * CommEvent = dynamic_cast<CommunicationEvent *> (ev);
	    if( CommEvent->CommType == RETRIEVE_ID &&
	        CommEvent->reply == true ){
            	nodeIDs.insert(nodeIDs.begin() + node, *(std::string*)CommEvent->payload);
	    }
        delete ev;
    } else if (dynamic_cast<CompletionEvent*>(ev)) {
        CompletionEvent *event = dynamic_cast<CompletionEvent*>(ev);
        int jobNum = event->jobNum;
        if (NULL == runningJobs[jobNum].ai) {
            runningJobs.erase(jobNum);
            delete ev;
            return; //this event has already stopped (probably faulted)
        }

        if (0 == (--(runningJobs[jobNum].i))) {
            runningJobs[jobNum].ai -> job -> hasRun = true;
            if( printJobLog ){
                logJobFinish( runningJobs[ jobNum ] );
            }
            finishingcomp.push_back(event->copy());
            FinalTimeEvent *fte = new FinalTimeEvent();
            if(runningJobs[jobNum].ai->job->getStartTime() == getCurrentSimTime())
                fte->forceExecute = true;
            selfLink->send(0, fte); //send back an event at the same time so we know it finished 
        }
        if (jobNum == jobs.back().jobNum) {
            if (jobs.empty()) {
                unregisterYourself();
            }
        }
        delete ev;
    } else {
        //If it is not a completion it is (hopefully) a fault.
        //as far as the scheduler is concerned this is treated the same way
        //however, must send a kill event to tell all nodes with this job to
        //stop running (not all the nodes may have failed but all must stop)
        //for now, the job is dropped forever
        FaultEvent *event = dynamic_cast<FaultEvent*>(ev);
        if (event) {
            int jobNum = event->jobNum;
            if (jobNum == -1) {
                return; // the node that faulted was not running a job, nothing for 
                //scheduler, allocator, machine to do
                delete ev;
            }
            //force all nodes running the job to kill it


            AllocInfo *ai = runningJobs[jobNum].ai;
            if (ai == NULL)
            {
                runningJobs.erase(jobNum);
                delete ev;
                return;
            }
            //if ai is NULL we already killed this job

            runningJobs[jobNum].ai -> job -> hasRun = true;

            if (printJobLog) {
                logJobFault(runningJobs[jobNum], event);
            }

            for (int i = 0; i < ai -> job -> getProcsNeeded(); ++i) {
                JobKillEvent *ec = new JobKillEvent(ai -> job -> getJobNum());

                nodes[ai -> nodeIndices[i]] -> send(ec); 
            }

            machine -> deallocate(ai);
            theAllocator -> deallocate(ai);
            if (useYumYumTraceFormat) {
                stats->jobFinishes(ai, getCurrentSimTime() + 1);
                scheduler->jobFinishes(ai->job, getCurrentSimTime() + 1, machine);
            } else {
                stats->jobFinishes(ai, getCurrentSimTime() );
                scheduler->jobFinishes(ai->job, getCurrentSimTime() , machine);
            }
            //the job is done and deleted from our records; don't need
            delete runningJobs.find( jobNum )->second.ai; 
            //its allocinfo again
            runningJobs.erase(jobNum);

            startNextJob();

            if (jobNum == jobs.back().jobNum) {
                while (!jobs.empty() && 
                       runningJobs.find(jobs.back().jobNum) == runningJobs.end() && 
                       jobs.back().hasRun == true) {
                    jobs.pop_back();
                }
                if (jobs.empty()) {
                    unregisterYourself();             // This is the one that actually does the unregistering.
                }
            }

            delete ev;
        } else {
            schedout.fatal(CALL_INFO, 1, "S: Error! Bad Event Type!\n");
        }
    }  
}


void schedComponent::handleJobArrivalEvent(Event *ev) 
{
    schedout.debug(CALL_INFO, 4, 0, "arrival event\n");
    if (NULL != dynamic_cast<CommunicationEvent*>(ev)) {
        schedout.debug(CALL_INFO, 4, 0, "comm event\n");
        CommunicationEvent * CommEvent = dynamic_cast<CommunicationEvent *>(ev);

        if (CommEvent->CommType == LOG_JOB_START) {
        } else if (CommEvent -> CommType == START_FILE_WATCH) {
        } else if (CommEvent -> CommType == START_NEXT_JOB) {
            bool startingNewJob = true;
                while (startingNewJob) {
                AllocInfo* newai = scheduler -> tryToStart(theAllocator, getCurrentSimTime(), machine, stats);
                startingNewJob = (NULL != newai);
                if (FSTtype > 0 && startingNewJob) {
                    calcFST -> jobStarts(newai -> job, getCurrentSimTime());
                }
            }
        }
        delete ev;
        return;
    }

    ArrivalEvent *arevent = dynamic_cast<ArrivalEvent*>(ev);
    if (NULL != arevent) {
        finishingarr.push_back(arevent);
        FinalTimeEvent* fte = new FinalTimeEvent();
        if (useYumYumSimulationKill xor YumYumSimulationKillFlag) {
            fte -> forceExecute = true; // avoid intermittent hangs when using yumyum
        }
        else {
            fte -> forceExecute = false; // otherwise keep previous behavior
        }
        //send back an event at the same time so we know it finished 
        selfLink -> send(0, fte); 
    } else {
        FinalTimeEvent* fev = dynamic_cast<FinalTimeEvent*>(ev);
        if (NULL != fev) {
            if (lastfinaltime == getCurrentSimTime() && !fev -> forceExecute) {
                delete ev;
                return; //ignore duplicate final time events 
                //(unless the event forces us to handle them)
            } else {
                lastfinaltime = getCurrentSimTime();
            }
            if (finishingcomp.size() > 1) {
                //finishingcomp.sort(compareCEPointers); 
            }

            while (!finishingcomp.empty()) {
                int finishedJobNum = finishingcomp.front() -> jobNum;
                AllocInfo *ai = runningJobs[finishedJobNum].ai;
                runningJobs.erase(finishedJobNum);
                machine -> deallocate(ai);
                theAllocator -> deallocate(ai);
                if (useYumYumTraceFormat) {
                    stats -> jobFinishes(ai, getCurrentSimTime() + 1);
                    scheduler -> jobFinishes(ai -> job, getCurrentSimTime() + 1, machine);
                } else {
                    if (FSTtype > 0) calcFST -> jobCompletes(ai -> job);
                    stats -> jobFinishes(ai, getCurrentSimTime());
                    scheduler -> jobFinishes(ai -> job, getCurrentSimTime(), machine);
                }
                delete ai;

                if (finishedJobNum == jobs.back().jobNum) {
                    while (!jobs.empty() && 
                           runningJobs.find(jobs.back().jobNum) == runningJobs.end() && 
                           jobs.back().hasRun == true) {
                        jobs.pop_back();
                    }

                    if (jobs.empty()) {
                        unregisterYourself();
                    }
                }
                delete finishingcomp.front();
                finishingcomp.pop_front();
            } 
            //arrivals are handled strictly after finishes at the same time to
            //match the Java simulator
            //each time step these are cleared so we don't need to worry about
            //events not at the same time step, and they should already be sorted
            //by number because they are given to SST (and therefore come back) that way.
            while (!finishingarr.empty()) {
                Job* arrivingjob = &jobs[finishingarr.front() -> getJobIndex()];
                if (FSTtype == 2) { 
                    //relaxed, so do FST before we tell the scheduler about the job
                    calcFST -> jobArrives(arrivingjob, scheduler, machine);
                    finishingarr.front() -> happen(machine, theAllocator, 
                                                   scheduler, stats, arrivingjob);
                } else if (FSTtype == 1){
                    finishingarr.front() -> happen(machine, theAllocator, 
                                                   scheduler, stats, arrivingjob);
                    calcFST -> jobArrives(arrivingjob, scheduler, machine);
                } else {
                    finishingarr.front() -> happen(machine, theAllocator, 
                                                   scheduler, stats, arrivingjob);
                }
                delete finishingarr.front();
                finishingarr.pop_front();
            }      
            delete ev; 

            startNextJob();
        } else {
            schedout.fatal(CALL_INFO, 1, "Arriving event was not an arrival nor finaltime event");
        }
    }
    schedout.debug(CALL_INFO, 4, 0, "finishing arrival event\n");
}


void schedComponent::finish() 
{
    scheduler -> done();
    stats -> done();
    theAllocator -> done();
}


void schedComponent::startJob(AllocInfo* ai) 
{
    Job* j = ai->job;
    int* jobNodes = ai->nodeIndices;
    double communicationTime = 0;
    unsigned long actualRunningTime = j->getActualTime();
    
    //calculate running time with communication overhead
    if (timePerDistance -> at(0) != 0 && NULL != (MachineMesh*)machine && NULL != (MeshAllocInfo*) ai) { 
        if (NULL != ((MeshAllocInfo*)ai) -> processors) {
        
            long baselineL1Distance = ((MachineMesh*)machine)->baselineL1Distance(j);
            double randomNumber = rng->nextUniform() * 0.8 - 0.4;
            int numberOfProcs = ((MeshAllocInfo*)ai)->processors->size();
                     
            //running time w/o any communication overhead (baseline):
            communicationTime = baselineL1Distance; //TODO: timePerDistance->at(3) * baselineL1Distance;
            unsigned long baselineRunningTime = actualRunningTime / (timePerDistance->at(0) + timePerDistance->at(1) *
                                                                     communicationTime); //TODO: ((double)timePerDistance->at(2) + communicationTime));
            unsigned long averagePairwiseDistance;
            //allocated hop distance
            if (numberOfProcs > 1) { 
                averagePairwiseDistance = 2 * ((MachineMesh*)machine)->pairwiseL1Distance( ((MeshAllocInfo*)ai)->processors)/ (numberOfProcs * (numberOfProcs - 1));
            } else {
                averagePairwiseDistance = 0;
            }
            double additiveTerm = timePerDistance->at(4) * averagePairwiseDistance * randomNumber;
            communicationTime = timePerDistance->at(3) * averagePairwiseDistance; 
            //new communication overhead
            actualRunningTime = timePerDistance->at(0) * baselineRunningTime
                + timePerDistance->at(1) * 
                  ((double)timePerDistance->at(2) + communicationTime) * 
                  baselineRunningTime;
            //randomization
            if (actualRunningTime >= -additiveTerm) {
                actualRunningTime += additiveTerm;
            }
        } 
    }

    if (actualRunningTime > j->getEstimatedRunningTime()) {
        schedout.fatal(CALL_INFO, 1, "Job %lu has running time %lu, which is longer than estimated running time %lu\n", j -> getJobNum(), actualRunningTime, j->getEstimatedRunningTime());
        actualRunningTime = j->getEstimatedRunningTime();
    }

    // send to each job in the node list
    for (int i = 0; i < j->getProcsNeeded(); ++i) {
        JobStartEvent *ec = new JobStartEvent(actualRunningTime, j->getJobNum());
        nodes[jobNodes[i]] -> send(ec); 
    }

    IAI iai;
    iai.i = j -> getProcsNeeded();
    iai.ai = ai;
    runningJobs[j -> getJobNum()] = iai;

    if (printJobLog) {
        logJobStart(iai);
    }
}


void schedComponent::logJobStart(IAI iai)
{
    if (!jobLog.is_open()) {
        jobLog.open( this->jobLogFileName.c_str(), ios::out );
        if (jobLog.is_open()) {
            jobLog << "jobid, begin, end, failed, compute_nodes" << endl;
            // writing the header here should be safe, since the jobs should start before they end
        }
    }

    if (jobLog.is_open()) {
        jobLog << *iai.ai->job->getID() << "," << getCurrentSimTime() << ",-1,-1,";
        for( int counter = 0; counter < iai.ai->job->getProcsNeeded(); counter ++ ){
            if (counter > 0) {
                jobLog << " ";
            }
            jobLog << nodeIDs.at( iai.ai->nodeIndices[ counter ] );
        }
        jobLog << endl;
    } else {
        schedout.fatal(CALL_INFO, 1, "Couldn't open %s for writing the job log.", this->jobLogFileName.c_str());
    }
}

void schedComponent::logJobFinish(IAI iai)
{
    if (!jobLog.is_open()) {
        jobLog.open( this->jobLogFileName.c_str(), ios::out );
    }

    if (jobLog.is_open()) {
        jobLog << *iai.ai->job->getID() << "," << iai.ai->job->getStartTime() << "," << getCurrentSimTime() << ",0,";
        for( int counter = 0; counter < iai.ai->job->getProcsNeeded(); counter ++ ){
            if (counter > 0) {
                jobLog << " ";
            }
            jobLog << nodeIDs.at( iai.ai->nodeIndices[ counter ] );
        }
        jobLog << endl;
    } else {
        schedout.fatal(CALL_INFO, 1, "Couldn't open %s for writing the job log.", this->jobLogFileName.c_str());
    } 

    if (getCurrentSimTime() < iai.ai->job->getStartTime()) {
        schedout.fatal(CALL_INFO, 1, "Job begin time larger than end time: jobid=%s, begin=%lu, end=%"PRIu64"\n", (*iai.ai -> job -> getID()).c_str(), iai.ai -> job -> getStartTime(), getCurrentSimTime());
    }
}

void schedComponent::logJobFault(IAI iai, FaultEvent * faultEvent)
{
    if (!jobLog.is_open()) {
        jobLog.open( this->jobLogFileName.c_str(), ios::out );
    }

    if (jobLog.is_open()) {
        jobLog << *iai.ai->job->getID() << "," << iai.ai->job->getStartTime() << "," << getCurrentSimTime() << ",1,";
        for( int counter = 0; counter < iai.ai->job->getProcsNeeded(); counter ++ ){
            if (counter > 0) {
                jobLog << " ";
            }
            jobLog << nodeIDs.at( iai.ai->nodeIndices[ counter ] );
        }

        jobLog << endl;
    }else{
        schedout.fatal(CALL_INFO, 1, "Couldn't open %s for writing the job log.", this->jobLogFileName.c_str() );
    } 

    if (getCurrentSimTime() < iai.ai -> job -> getStartTime()) {
        schedout.fatal(CALL_INFO, 1, "Job begin time larger than end time: jobid=%s, begin=%lu, end=%"PRIu64"\n", (*iai.ai -> job -> getID()).c_str(), iai.ai -> job -> getStartTime(), (uint64_t)getCurrentSimTime());
    }
}

// Element Libarary / Serialization stuff

//BOOST_CLASS_EXPORT(ArrivalEvent)
BOOST_CLASS_EXPORT(schedComponent)

