#!/bin/bash
# spawn.sh

# Spawns a lot of instances of cmsRun Hcal*.py on whatever machines you like
# Use the -c flag to clear the data directory beforehand.

# Example of use:
#
#    ./spawn.sh -c 10 jtdinsmo t3btch043.mit.edu t3desk014.mit.edu
#
# will clear the data directory and spawn 10 clients each on t3btch043.mit.edu 
# and t3desk014.mit.edu. These clients will run in their own subprocess until they
# quit, but they will continue to print output onto the terminal that you ran the 
# ./spawn.sh from. To determine if they are still running, you can run `ps` on 
# from the machine that you ran ./spawn.sh from, and every instance of `sshpass`
# is a client. Or, you can run ps -e on any of the machines you listed, and every
# instance of cmsRun will be a client running on that specific machine.

# So for the above input, ps will register 20 instances of sshpass on whatever
# machine you ran the command on, and ps -e will register 10 instances of cmsRun
# each on t3btch043.mit.edu and t3desk014.mit.edu.


# Determine whether the -c flag was used
getopts 'c' flag
clearSet=0

case ${flag} in
    c ) clearSet=1;;
esac

# Check to make sure the number of clients was given
if [ "$#" -le "$(($clearSet+1))" ]; then
    echo "Please provide the number of clients to spawn and your username, in that order."
    exit
fi

numClients="$((1+$clearSet))"
timetoRun=$(date "+0 mins" +%s)
timeNow=$(date %s)
pathToPython="$(pwd)"

echo ${!numClients}
# Clear the data directory if -c was used
if [ $clearSet == 1 ]; then
    echo "Clearing data directory"
    rm data/*.dat
fi

mkdir -p data/${!numClients}_${!numClients}
source /cvmfs/cms.cern.ch/cmsset_default.sh
eval `scramv1 runtime -sh`

#echo "Starting host ${!hostNum}"
for ((i=0; i < ${!numClients}; i++))
  do
    log="data/${nCPUS}_${nperCPU}_${i}.log"
    sleep_seconds=$(echo "$timetoRun - $timeNow")
    date >$log
    sleep $sleep_seconds
    echo "Sleeping $sleep_seconds seconds" > $log
    date >$log
    cmsRun OnLine_HLT_GRun.py > $log &
    echo "started"
  done

echo "Done"


