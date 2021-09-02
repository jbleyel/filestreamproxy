/*
 * SessionMap.cpp
 *
 *  Created on: 2015. 12. 10.
 *      Author: oskwon
 */
 
#include <stdio.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <string.h>

#include "Logger.h"
#include "Util.h"
#include "SessionMap.h"

using namespace std;

SessionMap* SessionMap::instance = 0;

//-------------------------------------------------------------------------------
SessionMap::SessionMap() throw(trap)
{
	mSemId = 0;
	mShmFd = 0;
	mShmData = 0;
	max_encoder_count = Util::get_encoder_count();

	mSemName = "/tsp_session_sem";
	mShmName = "/tsp_session_shm";
	mShmSize = sizeof(SessionInfo) * max_encoder_count;

	if (Open() == false)
		throw(trap("session ctrl init fail."));
	DEBUG("shm-info : fd [%d], name [%s], size [%d], data [%p]", mShmFd, mShmName.c_str(), mShmSize, mShmData);
	DEBUG("sem-info : id [%p], name [%s]", mSemId, mSemName.c_str());

}
//-------------------------------------------------------------------------------

int SessionMap::get_pid_by_ip(std::string aIpAddr) 
{
	for (int i = 0; i < max_encoder_count; i++) {
		if (strcmp(mShmData[i].ip, aIpAddr.c_str()) == 0)
			return mShmData[i].pid;
	}
	return 0;
}
//-------------------------------------------------------------------------------

void SessionMap::cleanup()
{
	std::vector<int> pidlist = Util::find_process_by_name("transtreamproxy", 0);

	Wait();
	for (int i = 0; i < max_encoder_count; i++) {
		if (mShmData[i].pid != 0) {
			int pid = mShmData[i].pid;
			if(terminated(pidlist, pid)) {
				erase(pid);
			}
		}
	}
	Post();
}
//-------------------------------------------------------------------------------

void SessionMap::dump(const char* aMessage)
{
	if (Logger::instance()->get_level() >= Logger::INFO) {
		DUMMY(" >> %s", aMessage);
		DUMMY("-------- [ DUMP HOST INFO ] ---------");
		for (int i = 0; i < max_encoder_count; i++) {
			DUMMY("%d : ip [%s], pid [%d]", i,  mShmData[i].ip, mShmData[i].pid);
		}
		DUMMY("-------------------------------------");
	}
}
//----------------------------------------------------------------------

bool SessionMap::terminated(std::vector<int>& aList, int aPid)
{
	for (int i = 0; i < aList.size(); ++i) {
		if (aList[i] == aPid) {
			return false;
		}
	}
	return true;
}
//----------------------------------------------------------------------

int SessionMap::add(std::string aIpAddr, int aPid)
{
	int i = 0;
	bool result = false;

	Wait();
	for (; i < max_encoder_count; i++) {
		if (mShmData[i].pid == 0) {
			result = true;
			mShmData[i].pid = aPid;
			strcpy(mShmData[i].ip, aIpAddr.c_str());
			break;
		}
	}
	Post();
	dump("after register.");

	return result ? i : -1;
}
//----------------------------------------------------------------------

void SessionMap::remove(std::string aIpAddr)
{
	Wait();
	for (int i = 0; i < max_encoder_count; i++) {
		if (strcmp(mShmData[i].ip, aIpAddr.c_str()) == 0) {
			memset(mShmData[i].ip, 0, 16);
			mShmData[i].pid = 0;
			break;
		}
	}
	Post();
	dump("after unregister.");
}
//----------------------------------------------------------------------

void SessionMap::erase(int aPid)
{
	for (int i = 0; i < max_encoder_count; i++) {
		if (mShmData[i].pid == aPid) {
			DEBUG("erase.. %s : %d", mShmData[i].ip, mShmData[i].pid);
			memset(mShmData[i].ip, 0, 16);
			mShmData[i].pid = 0;
			break;
		}
	}
}
//----------------------------------------------------------------------

int SessionMap::update(std::string aIpAddr, int aPid)
{
	int i = 0;
	bool result = false;

	dump("before update.");
	Wait();
	for (; i < max_encoder_count; i++) {
		if (strcmp(mShmData[i].ip, aIpAddr.c_str()) == 0) {
			result = true;
			Util::kill_process(mShmData[i].pid);
			memset(mShmData[i].ip, 0, 16);
			mShmData[i].pid = 0;
			break;
		}
	}
	Post();
	add(aIpAddr, aPid);
	return result ? i : -1;
}
//----------------------------------------------------------------------

int SessionMap::already_exist(std::string aIpAddr)
{
	int existCount = 0;
	Wait();
	for (int i = 0; i < max_encoder_count; i++) {
		if (strcmp(mShmData[i].ip, aIpAddr.c_str()) == 0) {
			existCount++;
		}
	}
	Post();
	return existCount;
}
//----------------------------------------------------------------------

void SessionMap::post()
{
	Post();
}
//----------------------------------------------------------------------

