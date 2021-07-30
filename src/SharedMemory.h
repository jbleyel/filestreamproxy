/*
 * SharedMemory.h
 *
 *  Created on: 2014. 6. 12.
 *      Author: oskwon
 */

#ifndef SHAREDMEMORY_H_
#define SHAREDMEMORY_H_

#include <string>

#include <fcntl.h>
#include <unistd.h>
#include <semaphore.h>
#include <sys/mman.h>

#include "Logger.h"

using namespace std;
//-------------------------------------------------------------------------------

template <class T>
class SharedMemory
{
protected:
	sem_t* mSemId;
	std::string mSemName;

	int mShmFd;
	int mShmSize;
	std::string mShmName;

	T* mShmData;

protected:
	void Close()
	{
		if (mShmData != 0) {
			munmap(mShmData, mShmSize);
		}
		mShmData = 0;
		if (mShmFd > 0) {
			close(mShmFd);
			//shm_unlink(mShmName.c_str());
		}
		mShmFd = 0;
		if (mSemId != 0) {
			sem_close(mSemId);
			sem_unlink(mSemName.c_str());
		}
		mSemId = 0;
	}

	bool Open()
	{
		mShmFd = shm_open(mShmName.c_str(), O_CREAT | O_RDWR, S_IRWXU | S_IRWXG);
		if (mShmFd < 0) {
			return false;
		}
		ftruncate(mShmFd, mShmSize);

		mShmData = (T*) mmap(NULL, mShmSize, PROT_READ | PROT_WRITE, MAP_SHARED, mShmFd, 0);
		if (mShmData == 0) {
			return false;
		}
		mSemId = sem_open(mSemName.c_str(), O_CREAT, S_IRUSR | S_IWUSR, 1);
		return true;
	}

	void Wait()
	{
		DEBUG("WAIT-BEFORE");
		sem_wait(mSemId);
		DEBUG("WAIT-AFTER");
	}
	void Post()
	{
		DEBUG("POST-BEFORE");
		sem_post(mSemId);
		DEBUG("POST-AFTER");
	}

public:
	~SharedMemory()
	{
		Close();
	}
};
//-------------------------------------------------------------------------------

#endif /* UPOSIXSHAREDMEMORY_H_ */
