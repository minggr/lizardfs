/*
   Copyright 2005-2010 Jakub Kruszona-Zawadzki, Gemius SA, 2013 Skytechnology sp. z o.o..

   This file was part of MooseFS and is part of LizardFS.

   LizardFS is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, version 3.

   LizardFS is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with LizardFS  If not, see <http://www.gnu.org/licenses/>.
 */

#include "config.h"

#include "master/chunks.h"

#include <inttypes.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <syslog.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#ifdef METARESTORE
#  include <time.h>
#endif
#include <algorithm>

#include "common/chunks_availability_state.h"
#include "common/datapack.h"
#include "common/goal.h"
#include "common/lizardfs_version.h"
#include "common/massert.h"
#include "common/MFSCommunication.h"
#include "master/chunk_copies_calculator.h"
#include "master/filesystem.h"

#ifndef METARESTORE
#  include "common/cfg.h"
#  include "common/main.h"
#  include "master/matoclserv.h"
#  include "master/matocsserv.h"
#  include "common/random.h"
#  include "master/topology.h"
#endif

#define USE_SLIST_BUCKETS 1
#define USE_FLIST_BUCKETS 1
#define USE_CHUNK_BUCKETS 1

#define MINLOOPTIME 1
#define MAXLOOPTIME 7200
#define MAXCPS 10000000
#define MINCPS 10000

#define HASHSIZE 0x100000
#define HASHPOS(chunkid) (((uint32_t)chunkid)&0xFFFFF)

#ifndef METARESTORE

/* chunk.operation */
enum {NONE,CREATE,SET_VERSION,DUPLICATE,TRUNCATE,DUPTRUNC};
/* slist.valid */
/* INVALID - wrong version / or got info from chunkserver (IO error etc.)  ->  to delete */
/* DEL - deletion in progress */
/* BUSY - operation in progress */
/* VALID - ok */
/* TDBUSY - to delete + BUSY */
/* TDVALID - want to be deleted */
enum {INVALID,DEL,BUSY,VALID,TDBUSY,TDVALID};

/* List of servers containing the chunk */
struct slist {
	void *ptr; // server data as matocsserventry
	uint32_t version;
	ChunkType chunkType;
	uint8_t valid;
//	uint8_t sectionid; - idea - Split machines into sctions. Try to place each copy of particular chunk in different section.
//	uint16_t machineid; - idea - If there are many different processes on the same physical computer then place there only one copy of chunk.
	slist *next;
	slist()
			: ptr(nullptr),
			  version(0),
			  chunkType(ChunkType::getStandardChunkType()),
			  valid(INVALID),
			  next(nullptr) {
	}

	bool is_busy() const {
		return valid == BUSY || valid == TDBUSY;
	}

	bool is_valid() const {
		return valid != INVALID && valid != DEL;
	}

	bool is_todel() const {
		return valid == TDVALID || valid == TDBUSY;
	}

	void mark_busy() {
		switch (valid) {
		case VALID:
			valid = BUSY;
			break;
		case TDVALID:
			valid = TDBUSY;
			break;
		default:
			sassert(!"slist::mark_busy(): wrong state");
		}
	}
	void unmark_busy() {
		switch (valid) {
		case BUSY:
			valid = VALID;
			break;
		case TDBUSY:
			valid = TDVALID;
			break;
		default:
			sassert(!"slist::unmark_busy(): wrong state");
		}
	}
	void mark_todel() {
		switch (valid) {
		case VALID:
			valid = TDVALID;
			break;
		case BUSY:
			valid = TDBUSY;
			break;
		default:
			sassert(!"slist::mark_todel(): wrong state");
		}
	}
	void unmark_todel() {
		switch (valid) {
		case TDVALID:
			valid = VALID;
			break;
		case TDBUSY:
			valid = BUSY;
			break;
		default:
			sassert(!"slist::unmark_todel(): wrong state");
		}
	}
};

#ifdef USE_SLIST_BUCKETS
#define SLIST_BUCKET_SIZE 5000

struct slist_bucket {
	slist bucket[SLIST_BUCKET_SIZE];
	uint32_t firstfree;
	slist_bucket *next;
	slist_bucket() : firstfree(0) {
	}
};

static slist_bucket *sbhead = NULL;
static slist *slfreehead = NULL;
#endif /* USE_SLIST_BUCKET */

#endif /* METARESTORE */

#ifndef METARESTORE
static inline slist* slist_malloc();
static inline void slist_free(slist *p);
#endif

class chunk {
public:
	uint64_t chunkid;
	chunk *next;
	uint32_t *ftab;
#ifndef METARESTORE
	slist *slisthead;
#endif
	uint32_t version;
	uint32_t lockid;
	uint32_t lockedto;
	uint32_t fcount;
	uint8_t goal;
#ifndef METARESTORE
	uint8_t needverincrease:1;
	uint8_t interrupted:1;
	uint8_t operation:4;
private:
	uint8_t goalInStats_;
	uint8_t allMissingParts_, regularMissingParts_;
	uint8_t allRedundantParts_, regularRedundantParts_;
	uint8_t allStandardCopies_, regularStandardCopies_;
	uint8_t allAvailabilityState_, regularAvailabilityState_;
public:
	static ChunksAvailabilityState allChunksAvailability, regularChunksAvailability;
	static ChunksReplicationState allChunksReplicationState, regularChunksReplicationState;
	static uint64_t count;
	static uint64_t allStandardChunkCopies[11][11], regularStandardChunkCopies[11][11];

	/*
	 * This method should be called on a new chunk
	 */
	void initStats() {
		count++;
		allMissingParts_ = regularMissingParts_ = 0;
		allRedundantParts_ = regularRedundantParts_ = 0;
		allStandardCopies_ = regularStandardCopies_ = 0;
		allAvailabilityState_ = regularAvailabilityState_ = ChunksAvailabilityState::kSafe;
		goalInStats_ = 0;
		addToStats();
		updateStats();
	}

	/*
	 * This method should be called when chunk is removed
	 */
	void freeStats() {
		count--;
		removeFromStats();
	}

	/*
	 * Updates statistics of all chunks
	 */
	void updateStats() {
		removeFromStats();
		allStandardCopies_ = regularStandardCopies_ = 0;
		ChunkCopiesCalculator all(goal), regular(goal);
		for (slist* s = slisthead; s != nullptr; s = s->next) {
			if (!s->is_valid()) {
				continue;
			}
			all.addPart(s->chunkType);
			if (s->chunkType.isStandardChunkType() && allStandardCopies_ < 10) {
				allStandardCopies_++;
			}
			if (!s->is_todel()) {
				regular.addPart(s->chunkType);
				if (s->chunkType.isStandardChunkType() && regularStandardCopies_ < 10) {
					regularStandardCopies_++;
				}
			}
		}
		allAvailabilityState_ = all.getState();
		allMissingParts_ = std::min(200U, all.countPartsToRecover());
		allRedundantParts_ = std::min(200U, all.countPartsToRemove());
		regularAvailabilityState_ = regular.getState();
		regularMissingParts_ = std::min(200U, regular.countPartsToRecover());
		regularRedundantParts_ = std::min(200U, regular.countPartsToRemove());
		addToStats();
	}

	bool isSafe() const {
		return allAvailabilityState_ == ChunksAvailabilityState::kSafe;
	}

	bool isEndangered() const {
		return allAvailabilityState_ == ChunksAvailabilityState::kEndangered;
	}

	bool isLost() const {
		return allAvailabilityState_ == ChunksAvailabilityState::kLost;
	}

	bool needsReplication() const {
		return regularMissingParts_ > 0;
	}

	bool needsDeletion() const {
		return regularRedundantParts_ > 0;
	}

	uint8_t getStandardCopiesCount() const {
		return allStandardCopies_;
	}

	bool is_locked() const {
		return lockedto >= main_time();
	}

	void copy_has_wrong_version(slist *s) {
		s->valid = INVALID;
		updateStats();
	}

	void invalidate_copy(slist *s) {
		s->valid = INVALID;
		s->version = 0;
		updateStats();
	}

	void delete_copy(slist *s) {
		s->valid = DEL;
		updateStats();
	}

	void unlink_copy(slist *s, slist **prev_next) {
		*prev_next = s->next;
		slist_free(s);
		updateStats();
	}

	slist *add_copy_no_stats_update(void *ptr, uint8_t valid, uint32_t version, ChunkType type) {
		slist *s = slist_malloc();
		s->ptr = ptr;
		s->valid = valid;
		s->version = version;
		s->chunkType = type;
		s->next = slisthead;
		slisthead = s;
		return s;
	}

	slist *addCopy(void *ptr, uint8_t valid, uint32_t version, ChunkType type) {
		slist *s = add_copy_no_stats_update(ptr, valid, version, type);
		updateStats();
		return s;
	}

	ChunkCopiesCalculator makeRegularCopiesCalculator() const {
		ChunkCopiesCalculator calculator(goal);
		for (const slist *s = slisthead; s != nullptr; s = s->next) {
			if (s->is_valid() && !s->is_todel()) {
				calculator.addPart(s->chunkType);
			}
		}
		return calculator;
	}

private:
	void removeFromStats() {
		ChunksAvailabilityState::State chunkState;

		chunkState = static_cast<ChunksAvailabilityState::State>(allAvailabilityState_);
		allChunksAvailability.removeChunk(goalInStats_, chunkState);
		allChunksReplicationState.removeChunk(goalInStats_, allMissingParts_, allRedundantParts_);

		chunkState = static_cast<ChunksAvailabilityState::State>(regularAvailabilityState_);
		regularChunksAvailability.removeChunk(goalInStats_, chunkState);
		regularChunksReplicationState.removeChunk(goalInStats_,
				regularMissingParts_, regularRedundantParts_);

		if (goalInStats_ == 0 || isOrdinaryGoal(goalInStats_)) {
			uint8_t limitedGoal = std::min<uint8_t>(10, goalInStats_);
			allStandardChunkCopies[limitedGoal][allStandardCopies_]--;
			regularStandardChunkCopies[limitedGoal][regularStandardCopies_]--;
		}
	}

	void addToStats() {
		goalInStats_ = goal;
		ChunksAvailabilityState::State chunkState;

		chunkState = static_cast<ChunksAvailabilityState::State>(allAvailabilityState_);
		allChunksAvailability.addChunk(goalInStats_, chunkState);
		allChunksReplicationState.addChunk(goalInStats_, allMissingParts_, allRedundantParts_);

		chunkState = static_cast<ChunksAvailabilityState::State>(regularAvailabilityState_);
		regularChunksAvailability.addChunk(goalInStats_, chunkState);
		regularChunksReplicationState.addChunk(goalInStats_,
				regularMissingParts_, regularRedundantParts_);

		if (goalInStats_ == 0 || isOrdinaryGoal(goalInStats_)) {
			uint8_t limitedGoal = std::min<uint8_t>(10, goalInStats_);
			allStandardChunkCopies[limitedGoal][allStandardCopies_]++;
			regularStandardChunkCopies[limitedGoal][regularStandardCopies_]++;
		}
	}
#endif
};

#ifndef METARESTORE
ChunksAvailabilityState chunk::allChunksAvailability, chunk::regularChunksAvailability;
ChunksReplicationState chunk::allChunksReplicationState, chunk::regularChunksReplicationState;
uint64_t chunk::count = 0;
uint64_t chunk::allStandardChunkCopies[11][11] = {{0}};
uint64_t chunk::regularStandardChunkCopies[11][11] = {{0}};
#endif

#ifdef USE_CHUNK_BUCKETS
#define CHUNK_BUCKET_SIZE 20000
struct chunk_bucket {
	chunk bucket[CHUNK_BUCKET_SIZE];
	uint32_t firstfree;
	chunk_bucket *next;
};

static chunk_bucket *cbhead = NULL;
static chunk *chfreehead = NULL;
#endif /* USE_CHUNK_BUCKETS */

static chunk *chunkhash[HASHSIZE];
static uint64_t nextchunkid=1;
#define LOCKTIMEOUT 120

#define UNUSED_DELETE_TIMEOUT (86400*7)

#ifndef METARESTORE

static uint32_t ReplicationsDelayDisconnect=3600;
static uint32_t ReplicationsDelayInit=300;

static uint32_t MaxWriteRepl;
static uint32_t MaxReadRepl;
static uint32_t MaxDelSoftLimit;
static uint32_t MaxDelHardLimit;
static double TmpMaxDelFrac;
static uint32_t TmpMaxDel;
static uint32_t HashSteps;
static uint32_t HashCPS;
static double AcceptableDifference;

static uint32_t jobshpos;
static uint32_t jobsrebalancecount;
static uint32_t jobsnorepbefore;

static uint32_t starttime;

struct job_info {
	uint32_t del_invalid;
	uint32_t del_unused;
	uint32_t del_diskclean;
	uint32_t del_overgoal;
	uint32_t copy_undergoal;
};

struct loop_info {
	job_info done,notdone;
	uint32_t copy_rebalance;
};

static loop_info chunksinfo = {{0,0,0,0,0},{0,0,0,0,0},0};
static uint32_t chunksinfo_loopstart=0,chunksinfo_loopend=0;

#endif

static uint64_t lastchunkid=0;
static chunk* lastchunkptr=NULL;

#ifndef METARESTORE
static uint32_t stats_deletions=0;
static uint32_t stats_replications=0;

void chunk_stats(uint32_t *del,uint32_t *repl) {
	*del = stats_deletions;
	*repl = stats_replications;
	stats_deletions = 0;
	stats_replications = 0;
}

#endif

#ifndef METARESTORE
#ifdef USE_SLIST_BUCKETS
static inline slist* slist_malloc() {
	slist_bucket *sb;
	slist *ret;
	if (slfreehead) {
		ret = slfreehead;
		slfreehead = ret->next;
		return ret;
	}
	if (sbhead==NULL || sbhead->firstfree==SLIST_BUCKET_SIZE) {
		sb = new slist_bucket;
		passert(sb);
		sb->next = sbhead;
		sbhead = sb;
	}
	ret = (sbhead->bucket)+(sbhead->firstfree);
	sbhead->firstfree++;
	return ret;
}

static inline void slist_free(slist *p) {
	p->next = slfreehead;
	slfreehead = p;
}
#else /* USE_SLIST_BUCKETS */

static inline slist* slist_malloc() {
	return new slist;
}

static inline void slist_free(slist* p) {
	delete p;
}

#endif /* USE_SLIST_BUCKETS */
#endif /* !METARESTORE */

#ifdef USE_CHUNK_BUCKETS
static inline chunk* chunk_malloc() {
	chunk_bucket *cb;
	chunk *ret;
	if (chfreehead) {
		ret = chfreehead;
		chfreehead = ret->next;
		return ret;
	}
	if (cbhead==NULL || cbhead->firstfree==CHUNK_BUCKET_SIZE) {
		cb = (chunk_bucket*)malloc(sizeof(chunk_bucket));
		passert(cb);
		cb->next = cbhead;
		cb->firstfree = 0;
		cbhead = cb;
	}
	ret = (cbhead->bucket)+(cbhead->firstfree);
	cbhead->firstfree++;
	return ret;
}

static inline void chunk_free(chunk *p) {
	p->next = chfreehead;
	chfreehead = p;
}
#else /* USE_CHUNK_BUCKETS */

static inline chunk* chunk_malloc() {
	chunk *cu;
	cu = (chunk*)malloc(sizeof(chunk));
	passert(cu);
	return cu;
}

static inline void chunk_free(chunk* p) {
	free(p);
}

#endif /* USE_CHUNK_BUCKETS */

chunk* chunk_new(uint64_t chunkid) {
	uint32_t chunkpos = HASHPOS(chunkid);
	chunk *newchunk;
	newchunk = chunk_malloc();
#ifdef METARESTORE
	printf("N%" PRIu64 "\n",chunkid);
#endif
	newchunk->next = chunkhash[chunkpos];
	chunkhash[chunkpos] = newchunk;
	newchunk->chunkid = chunkid;
	newchunk->version = 0;
	newchunk->goal = 0;
	newchunk->lockid = 0;
	newchunk->lockedto = 0;
	newchunk->fcount = 0;
	newchunk->ftab = NULL;
#ifndef METARESTORE
	newchunk->needverincrease = 1;
	newchunk->interrupted = 0;
	newchunk->operation = NONE;
	newchunk->slisthead = NULL;
	newchunk->initStats();
#endif
	lastchunkid = chunkid;
	lastchunkptr = newchunk;
	return newchunk;
}

chunk* chunk_find(uint64_t chunkid) {
	uint32_t chunkpos = HASHPOS(chunkid);
	chunk *chunkit;
#ifdef METARESTORE
	printf("F%" PRIu64 "\n",chunkid);
#endif
	if (lastchunkid==chunkid) {
		return lastchunkptr;
	}
	for (chunkit = chunkhash[chunkpos] ; chunkit ; chunkit = chunkit->next ) {
		if (chunkit->chunkid == chunkid) {
			lastchunkid = chunkid;
			lastchunkptr = chunkit;
			return chunkit;
		}
	}
	return NULL;
}

#ifndef METARESTORE
void chunk_delete(chunk* c) {
	if (lastchunkptr==c) {
		lastchunkid=0;
		lastchunkptr=NULL;
	}
	c->freeStats();
	chunk_free(c);
}

uint32_t chunk_count(void) {
	return chunk::count;
}

void chunk_info(uint32_t *allchunks,uint32_t *allcopies,uint32_t *regularvalidcopies) {
	uint32_t i,j,ag,rg;
	*allchunks = chunk::count;
	*allcopies = 0;
	*regularvalidcopies = 0;
	for (i=1 ; i<=10 ; i++) {
		ag=0;
		rg=0;
		for (j=0 ; j<=10 ; j++) {
			ag += chunk::allStandardChunkCopies[j][i];
			rg += chunk::regularStandardChunkCopies[j][i];
		}
		*allcopies += ag*i;
		*regularvalidcopies += rg*i;
	}
}

uint32_t chunk_get_missing_count(void) {
	uint32_t res = 0;
	for (int goal = kMinOrdinaryGoal; goal <= kMaxOrdinaryGoal; ++goal) {
		res += chunk::allChunksAvailability.lostChunks(goal);
	}
	for (int level = kMinXorLevel; level <= kMaxXorLevel; ++level) {
		res += chunk::allChunksAvailability.lostChunks(xorLevelToGoal(level));
	}
	return res;
}

void chunk_store_chunkcounters(uint8_t *buff, uint8_t matrixid) {
	uint8_t i,j;
	if (matrixid==0) {
		for (i=0 ; i<=10 ; i++) {
			for (j=0 ; j<=10 ; j++) {
				put32bit(&buff, chunk::allStandardChunkCopies[i][j]);
			}
		}
	} else if (matrixid==1) {
		for (i=0 ; i<=10 ; i++) {
			for (j=0 ; j<=10 ; j++) {
				put32bit(&buff, chunk::regularStandardChunkCopies[i][j]);
			}
		}
	} else {
		memset(buff,0,11*11*4);
	}
}
#endif

int chunk_change_file(uint64_t chunkid,uint8_t prevgoal,uint8_t newgoal) {
	chunk *c;
#ifndef METARESTORE
	uint8_t oldgoal;
#endif
	if (prevgoal==newgoal) {
		return STATUS_OK;
	}
	c = chunk_find(chunkid);
	if (c==NULL) {
		return ERROR_NOCHUNK;
	}
	if (c->fcount==0) {
#ifndef METARESTORE
		syslog(LOG_WARNING,"serious structure inconsistency: (chunkid:%016" PRIX64 ")",c->chunkid);
#else
		printf("serious structure inconsistency: (chunkid:%016" PRIX64 ")\n",c->chunkid);
#endif
		return ERROR_CHUNKLOST;	// ERROR_STRUCTURE
	}
#ifndef METARESTORE
	oldgoal = c->goal;
#endif
	if (c->fcount==1) {
		c->goal = newgoal;
	} else {
		if (c->ftab==NULL) {
			c->ftab = new uint32_t[kMaxOrdinaryGoal + 1];
			memset(c->ftab, 0, sizeof(uint32_t) * (kMaxOrdinaryGoal + 1));
			if (isOrdinaryGoal(c->goal)) {
				c->ftab[c->goal]=c->fcount-1;
			}
			if (isOrdinaryGoal(newgoal)) {
				c->ftab[newgoal]=1;
			}
			if (isOrdinaryGoal(c->goal) && isOrdinaryGoal(newgoal)) {
				if (newgoal > c->goal) {
					c->goal = newgoal;
				}
			}
		} else {
			if (isOrdinaryGoal(prevgoal)) {
				c->ftab[prevgoal]--;
			}
			if (isOrdinaryGoal(newgoal)) {
				c->ftab[newgoal]++;
			}
			if (isOrdinaryGoal(c->goal)) {
				c->goal = kMaxOrdinaryGoal;
				while (c->goal > kMinOrdinaryGoal && c->ftab[c->goal]==0) {
					c->goal--;
				}
			}
		}
	}
#ifndef METARESTORE
	if (oldgoal != c->goal) {
		c->updateStats();
	}
#endif
	return STATUS_OK;
}

static inline int chunk_delete_file_int(chunk *c,uint8_t goal) {
#ifndef METARESTORE
	uint8_t oldgoal;
#endif
	if (c->fcount==0) {
#ifndef METARESTORE
		syslog(LOG_WARNING,"serious structure inconsistency: (chunkid:%016" PRIX64 ")",c->chunkid);
#else
		printf("serious structure inconsistency: (chunkid:%016" PRIX64 ")\n",c->chunkid);
#endif
		return ERROR_CHUNKLOST;	// ERROR_STRUCTURE
	}
#ifndef METARESTORE
	oldgoal = c->goal;
#endif
	if (c->fcount==1) {
		c->goal = 0;
		c->fcount = 0;
#ifdef METARESTORE
		printf("D%" PRIu64 "\n",c->chunkid);
#endif
	} else {
		if (c->ftab) {
			if (isOrdinaryGoal(goal)) {
				c->ftab[goal]--;
			}
			if (isOrdinaryGoal(c->goal)) {
				c->goal = kMaxOrdinaryGoal;
				while (c->goal > kMinOrdinaryGoal && c->ftab[c->goal]==0) {
					c->goal--;
				}
			}
		}
		c->fcount--;
		if (c->fcount==1 && c->ftab) {
			delete[] c->ftab;
			c->ftab = NULL;
		}
	}
#ifndef METARESTORE
	if (oldgoal != c->goal) {
		c->updateStats();
	}
#endif
	return STATUS_OK;
}

static inline int chunk_add_file_int(chunk *c,uint8_t goal) {
#ifndef METARESTORE
	uint8_t oldgoal;
#endif
#ifndef METARESTORE
	oldgoal = c->goal;
#endif
	if (c->fcount==0) {
		c->goal = goal;
		c->fcount = 1;
	} else if (goal==c->goal) {
		c->fcount++;
		if (c->ftab && isOrdinaryGoal(goal)) {
			c->ftab[goal]++;
		}
	} else {
		if (c->ftab==NULL) {
			c->ftab = new uint32_t[kMaxOrdinaryGoal + 1];
			memset(c->ftab, 0, sizeof(uint32_t) * (kMaxOrdinaryGoal + 1));
			if (isOrdinaryGoal(c->goal)) {
				c->ftab[c->goal]=c->fcount;
			}
			if (isOrdinaryGoal(goal)) {
				c->ftab[goal]=1;
			}
			c->fcount++;
			if (isOrdinaryGoal(goal) && isOrdinaryGoal(c->goal)) {
				if (goal > c->goal) {
					c->goal = goal;
				}
			}
		} else {
			if (isOrdinaryGoal(goal)) {
				c->ftab[goal]++;
			}
			c->fcount++;
			if (isOrdinaryGoal(c->goal)) {
				c->goal = kMaxOrdinaryGoal;
				while (c->goal > kMinOrdinaryGoal && c->ftab[c->goal]==0) {
					c->goal--;
				}
			}
		}
	}
#ifndef METARESTORE
	if (oldgoal != c->goal) {
		c->updateStats();
	}
#endif
	return STATUS_OK;
}

int chunk_delete_file(uint64_t chunkid,uint8_t goal) {
	chunk *c;
	c = chunk_find(chunkid);
	if (c==NULL) {
		return ERROR_NOCHUNK;
	}
	return chunk_delete_file_int(c,goal);
}

int chunk_add_file(uint64_t chunkid,uint8_t goal) {
	chunk *c;
	c = chunk_find(chunkid);
	if (c==NULL) {
		return ERROR_NOCHUNK;
	}
	return chunk_add_file_int(c,goal);
}

int chunk_can_unlock(uint64_t chunkid, uint32_t lockid) {
	chunk *c;
	c = chunk_find(chunkid);
	if (c==NULL) {
		return ERROR_NOCHUNK;
	}
	if (lockid == 0) {
		// lockid == 0 -> force unlock
		return STATUS_OK;
	}
	// We will let client to unlock the chunk even if c->lockedto < main_time()
	// if he provides lockId that was used to lock the chunk -- this means that nobody
	// else used this chunk since it was locked (operations like truncate or replicate
	// would remove such a stale lock before modifying the chunk)
	if (c->lockid == lockid) {
		return STATUS_OK;
	} else if (c->lockedto == 0) {
		return ERROR_NOTLOCKED;
	} else {
		return ERROR_WRONGLOCKID;
	}
}

int chunk_unlock(uint64_t chunkid) {
	chunk *c;
	c = chunk_find(chunkid);
	if (c==NULL) {
		return ERROR_NOCHUNK;
	}
	// Don't remove lockid to safely accept retransmission of FUSE_CHUNK_UNLOCK message
	c->lockedto = 0;
	return STATUS_OK;
}

#ifndef METARESTORE

int chunk_get_validcopies(uint64_t chunkid, uint8_t *vcopies) {
	chunk *c;
	*vcopies = 0;
	c = chunk_find(chunkid);
	if (c==NULL) {
		return ERROR_NOCHUNK;
	}
	if (c->isLost()) {
		*vcopies = 0;
	} else if (c->isEndangered()) {
		*vcopies = 1;
	} else {
		// Safe chunk
		*vcopies = std::max<uint8_t>(2, c->getStandardCopiesCount());
	}
	return STATUS_OK;
}
#endif


#ifndef METARESTORE
int chunk_multi_modify(uint64_t *nchunkid, uint64_t ochunkid,
		uint8_t goal, uint8_t *opflag, uint32_t *lockid, bool usedummylockid) {
	slist *os,*s;
	uint32_t i;
#else
int chunk_multi_modify(uint32_t ts, uint64_t *nchunkid, uint64_t ochunkid,
		uint8_t goal, uint8_t opflag, uint32_t lockid) {
#endif
	chunk *oc,*c;

	if (ochunkid==0) {	// new chunk
#ifndef METARESTORE
		auto serversWithChunkTypes = matocsserv_getservers_for_new_chunk(goal);
		if (serversWithChunkTypes.empty()) {
			uint16_t uscount,tscount;
			double minusage,maxusage;
			matocsserv_usagedifference(&minusage,&maxusage,&uscount,&tscount);
			// if there are chunkservers and it's at least one minute after start then it means that there is no space left
			if (uscount > 0 && (uint32_t)(main_time()) > (starttime + 600)) {
				return ERROR_NOSPACE;
			} else {
				return ERROR_NOCHUNKSERVERS;
			}
		}
#endif
		c = chunk_new(nextchunkid++);
		c->version = 1;
#ifndef METARESTORE
		c->interrupted = 0;
		c->operation = CREATE;
#endif
		chunk_add_file_int(c,goal);
#ifndef METARESTORE
		for (i=0; i < serversWithChunkTypes.size(); ++i) {
			s = c->add_copy_no_stats_update(serversWithChunkTypes[i].first, BUSY,
					c->version, serversWithChunkTypes[i].second);
			matocsserv_send_createchunk(s->ptr, c->chunkid, s->chunkType, c->version);
		}
		c->updateStats();
		*opflag=1;
#endif
		*nchunkid = c->chunkid;
	} else {
		c = NULL;
		oc = chunk_find(ochunkid);
		if (oc==NULL) {
			return ERROR_NOCHUNK;
		}
#ifndef METARESTORE
		if (*lockid != 0 && *lockid != oc->lockid) {
			if (oc->lockid == 0 || oc->lockedto == 0) {
				// Lock was removed by some chunk operation or by a different client
				return ERROR_NOTLOCKED;
			} else {
				return ERROR_WRONGLOCKID;
			}
		}
		if (*lockid == 0 && oc->is_locked()) {
			return ERROR_LOCKED;
		}
		if (oc->isLost()) {
			return ERROR_CHUNKLOST;
		}
#endif

		if (oc->fcount==1) {	// refcount==1
			*nchunkid = ochunkid;
			c = oc;
#ifndef METARESTORE

			if (c->operation!=NONE) {
				return ERROR_CHUNKBUSY;
			}
			if (c->needverincrease) {
				i=0;
				for (s=c->slisthead ;s ; s=s->next) {
					if (s->is_valid()) {
						if (!s->is_busy()) {
							s->mark_busy();
						}
						s->version = c->version+1;
						matocsserv_send_setchunkversion(s->ptr, ochunkid, c->version+1, c->version,
								s->chunkType);
						i++;
					}
				}
				if (i>0) {
					c->interrupted = 0;
					c->operation = SET_VERSION;
					c->version++;
					*opflag=1;
				} else {
					// This should never happen - we verified this using ChunkCopiesCalculator
					return ERROR_CHUNKLOST;
				}
			} else {
				*opflag=0;
			}
#else
			if (opflag) {
				c->version++;
			}
#endif
		} else {
			if (oc->fcount==0) {	// it's serious structure error
#ifndef METARESTORE
				syslog(LOG_WARNING,"serious structure inconsistency: (chunkid:%016" PRIX64 ")",ochunkid);
#else
				printf("serious structure inconsistency: (chunkid:%016" PRIX64 ")\n",ochunkid);
#endif
				return ERROR_CHUNKLOST;	// ERROR_STRUCTURE
			}
#ifndef METARESTORE
			i=0;
			for (os=oc->slisthead ;os ; os=os->next) {
				if (os->is_valid()) {
					if (c==NULL) {
#endif
						c = chunk_new(nextchunkid++);
						c->version = 1;
#ifndef METARESTORE
						c->interrupted = 0;
						c->operation = DUPLICATE;
#endif
						chunk_delete_file_int(oc,goal);
						chunk_add_file_int(c,goal);
#ifndef METARESTORE
					}
					// TODO(msulikowski) implement COW of XOR chunks!
					s = c->add_copy_no_stats_update(os->ptr, BUSY, c->version,
							ChunkType::getStandardChunkType());
					matocsserv_send_duplicatechunk(s->ptr,c->chunkid,c->version,oc->chunkid,oc->version);
					i++;
				}
			}
			if (c!=NULL) {
				c->updateStats();
			}
			if (i>0) {
#endif
				*nchunkid = c->chunkid;
#ifndef METARESTORE
				*opflag=1;
			} else {
				return ERROR_CHUNKLOST;
			}
#endif
		}
	}

#ifndef METARESTORE
	c->lockedto = main_time() + LOCKTIMEOUT;
	if (*lockid == 0) {
		if (usedummylockid) {
			*lockid = 1;
		} else {
			*lockid = 2 + rndu32_ranged(0xFFFFFFF0); // some random number greater than 1
		}
	}
	c->lockid = *lockid;
#else
	c->lockedto=ts+LOCKTIMEOUT;
	c->lockid = lockid;
#endif
	return STATUS_OK;
}

#ifndef METARESTORE
int chunk_multi_truncate(uint64_t *nchunkid, uint64_t ochunkid, uint32_t length, uint8_t goal,
		bool truncatingUpwards) {
	slist *os,*s;
	uint32_t i;
#else
int chunk_multi_truncate(uint32_t ts,uint64_t *nchunkid,uint64_t ochunkid,uint8_t goal) {
#endif
	chunk *oc,*c;

	c=NULL;
	oc = chunk_find(ochunkid);
	if (oc==NULL) {
		return ERROR_NOCHUNK;
	}
#ifndef METARESTORE
	if (oc->is_locked()) {
		return ERROR_LOCKED;
	}
	oc->lockid = 0; // remove stale lock if exists
#endif
	if (oc->fcount==1) {	// refcount==1
		*nchunkid = ochunkid;
		c = oc;
#ifndef METARESTORE
		if (c->operation!=NONE) {
			return ERROR_CHUNKBUSY;
		}
		i=0;
		for (s=c->slisthead ;s ; s=s->next) {
			if (s->is_valid()) {
				if (!s->is_busy()) {
					s->mark_busy();
				}
				if (!truncatingUpwards
						&& s->chunkType.isXorChunkType()
						&& s->chunkType.isXorParity()
						&& (length % (MFSBLOCKSIZE * s->chunkType.getXorLevel()) != 0)) {
					syslog(LOG_WARNING, "Trying to truncate parity chunk: %016" PRIX64
							" - currently unsupported!!!", ochunkid);
					s->valid = INVALID;
					c->updateStats();
				} else {
					s->version = c->version+1;
					uint32_t chunkTypeLength =
							ChunkType::chunkLengthToChunkTypeLength(s->chunkType, length);
					matocsserv_send_truncatechunk(s->ptr, ochunkid, s->chunkType, chunkTypeLength,
							c->version + 1, c->version);
					i++;
				}
			}
		}
		if (i>0) {
			c->interrupted = 0;
			c->operation = TRUNCATE;
			c->version++;
		} else {
			return ERROR_CHUNKLOST;
		}
#else
		c->version++;
#endif
	} else {
		if (oc->fcount==0) {	// it's serious structure error
#ifndef METARESTORE
			syslog(LOG_WARNING,"serious structure inconsistency: (chunkid:%016" PRIX64 ")",ochunkid);
#else
			printf("serious structure inconsistency: (chunkid:%016" PRIX64 ")\n",ochunkid);
#endif
			return ERROR_CHUNKLOST;	// ERROR_STRUCTURE
		}
#ifndef METARESTORE
		i=0;
		// TODO add XOR chunks support
		for (os=oc->slisthead ;os ; os=os->next) {
			if (os->is_valid()) {
				if (c==NULL) {
#endif
					c = chunk_new(nextchunkid++);
					c->version = 1;
#ifndef METARESTORE
					c->interrupted = 0;
					c->operation = DUPTRUNC;
#endif
					chunk_delete_file_int(oc,goal);
					chunk_add_file_int(c,goal);
#ifndef METARESTORE
				}
				s = c->add_copy_no_stats_update(os->ptr, BUSY, c->version,
						ChunkType::getStandardChunkType());
				matocsserv_send_duptruncchunk(s->ptr,c->chunkid,c->version,oc->chunkid,oc->version,length);
				i++;
			}
		}
		if (c!=NULL) {
			c->updateStats();
		}
		if (i>0) {
#endif
			*nchunkid = c->chunkid;
#ifndef METARESTORE
		} else {
			return ERROR_CHUNKLOST;
		}
#endif
	}

#ifndef METARESTORE
	c->lockedto=(uint32_t)main_time()+LOCKTIMEOUT;
#else
	c->lockedto=ts+LOCKTIMEOUT;
#endif
	return STATUS_OK;
}

#ifndef METARESTORE
int chunk_repair(uint8_t goal,uint64_t ochunkid,uint32_t *nversion) {
	uint32_t bestversion;
	chunk *c;
	slist *s;

	*nversion=0;
	if (ochunkid==0) {
		return 0;	// not changed
	}

	c = chunk_find(ochunkid);
	if (c==NULL) {	// no such chunk - erase (nchunkid already is 0 - so just return with "changed" status)
		return 1;
	}
	if (c->is_locked()) { // can't repair locked chunks - but if it's locked, then likely it doesn't need to be repaired
		return 0;
	}
	c->lockid = 0; // remove stale lock if exists
	bestversion = 0;
	for (s=c->slisthead ; s ; s=s->next) {
		if (s->valid == VALID || s->valid == TDVALID || s->valid == BUSY || s->valid == TDBUSY) {	// found chunk that is ok - so return
			return 0;
		}
		if (s->valid == INVALID) {
			if (s->version>=bestversion) {
				bestversion = s->version;
			}
		}
	}
	if (bestversion==0) {	// didn't find sensible chunk - so erase it
		chunk_delete_file_int(c,goal);
		return 1;
	}
	c->version = bestversion;
	for (s=c->slisthead ; s ; s=s->next) {
		if (s->valid == INVALID && s->version==bestversion) {
			s->valid = VALID;
		}
	}
	*nversion = bestversion;
	c->updateStats();
	c->needverincrease=1;
	return 1;
}
#else
int chunk_set_version(uint64_t chunkid,uint32_t version) {
	chunk *c;
	c = chunk_find(chunkid);
	if (c==NULL) {
		return ERROR_NOCHUNK;
	}
	c->version = version;
	return STATUS_OK;
}
#endif

#ifndef METARESTORE
void chunk_emergency_increase_version(chunk *c) {
	slist *s;
	uint32_t i;
	i=0;
	for (s=c->slisthead ;s ; s=s->next) {
		if (s->is_valid()) {
			if (!s->is_busy()) {
				s->mark_busy();
			}
			s->version = c->version+1;
			matocsserv_send_setchunkversion(s->ptr, c->chunkid, c->version+1,c->version,
					s->chunkType);
			i++;
		}
	}
	if (i>0) {	// should always be true !!!
		c->interrupted = 0;
		c->operation = SET_VERSION;
		c->version++;
	} else {
		matoclserv_chunk_status(c->chunkid,ERROR_CHUNKLOST);
	}
	fs_incversion(c->chunkid);
}
#else
int chunk_increase_version(uint64_t chunkid) {
	chunk *c;
	c = chunk_find(chunkid);
	if (c==NULL) {
		return ERROR_NOCHUNK;
	}
	c->version++;
	return STATUS_OK;
}
#endif

#ifndef METARESTORE

const ChunksReplicationState& chunk_get_replication_state(bool regularChunksOnly) {
	return regularChunksOnly ?
			chunk::regularChunksReplicationState :
			chunk::allChunksReplicationState;
}

const ChunksAvailabilityState& chunk_get_availability_state(bool regularChunksOnly) {
	return regularChunksOnly ?
			chunk::regularChunksAvailability :
			chunk::allChunksAvailability;
}

typedef struct locsort {
	uint32_t ip;
	uint16_t port;
	uint32_t dist;
	uint32_t rnd;
} locsort;

int chunk_locsort_cmp(const void *aa,const void *bb) {
	const locsort *a = (const locsort*)aa;
	const locsort *b = (const locsort*)bb;
	if (a->dist<b->dist) {
		return -1;
	} else if (a->dist>b->dist) {
		return 1;
	} else if (a->rnd<b->rnd) {
		return -1;
	} else if (a->rnd>b->rnd) {
		return 1;
	}
	return 0;
}

struct ChunkLocation {
	ChunkLocation() : chunkType(ChunkType::getStandardChunkType()),
			distance(0), random(0) {
	}
	NetworkAddress address;
	ChunkType chunkType;
	uint32_t distance;
	uint32_t random;
	bool operator<(const ChunkLocation& other) const {
		if (distance < other.distance) {
			return true;
		} else if (distance > other.distance) {
			return false;
		} else {
			return random < other.random;
		}
	}
};

int chunk_getversionandlocations(uint64_t chunkid, uint32_t currentIp, uint32_t& version,
		uint32_t maxNumberOfChunkCopies, std::vector<ChunkTypeWithAddress>& serversList) {
	chunk *c;
	slist *s;
	uint8_t cnt;

	sassert(serversList.empty());
	c = chunk_find(chunkid);

	if (c == NULL) {
		return ERROR_NOCHUNK;
	}
	version = c->version;
	cnt = 0;
	std::vector<ChunkLocation> chunkLocation;
	ChunkLocation chunkserverLocation;
	for (s = c->slisthead; s; s = s->next) {
		if (s->is_valid()) {
			if (cnt < maxNumberOfChunkCopies && matocsserv_getlocation(s->ptr,
					&(chunkserverLocation.address.ip),
					&(chunkserverLocation.address.port)) == 0) {
				chunkserverLocation.distance =
						topology_distance(chunkserverLocation.address.ip, currentIp);
						// in the future prepare more sophisticated distance function
				chunkserverLocation.random = rndu32();
				chunkserverLocation.chunkType = s->chunkType;
				chunkLocation.push_back(chunkserverLocation);
				cnt++;
			}
		}
	}
	std::sort(chunkLocation.begin(), chunkLocation.end());
	for (uint i = 0; i < chunkLocation.size(); ++i) {
		const ChunkLocation& loc = chunkLocation[i];
		serversList.push_back(ChunkTypeWithAddress(loc.address, loc.chunkType));
	}
	return STATUS_OK;
}

void chunk_server_has_chunk(void *ptr, uint64_t chunkid, uint32_t version, ChunkType chunkType) {
	chunk *c;
	slist *s;
	const uint32_t new_version = version & 0x7FFFFFFF;
	const bool todel = version & 0x80000000;
	c = chunk_find(chunkid);
	if (c==NULL) {
		// chunkserver has nonexistent chunk, so create it for future deletion
		if (chunkid>=nextchunkid) {
			nextchunkid=chunkid+1;
		}
		c = chunk_new(chunkid);
		c->version = new_version;
		c->lockedto = (uint32_t)main_time()+UNUSED_DELETE_TIMEOUT;
		c->lockid = 0;
	}
	for (s=c->slisthead ; s ; s=s->next) {
		if (s->ptr == ptr && s->chunkType == chunkType) {
			// This server already notified us about its copy.
			// We normally don't get repeated notifications about the same copy, but
			// they can arrive after chunkserver configuration reload (particularly,
			// when folders change their 'to delete' status) or due to bugs.
			// Let's try to handle them as well as we can.
			switch (s->valid) {
			case DEL:
				// We requested deletion, but the chunkserver 'has' this copy again.
				// Repeat deletion request.
				c->invalidate_copy(s);
				// fallthrough
			case INVALID:
				// leave this copy alone
				return;
			default:
				break;
			}
			if (s->version != new_version) {
				syslog(LOG_WARNING, "chunk %016" PRIX64 ": master data indicated "
						"version %08" PRIX32 ", chunkserver reports %08"
						PRIX32 "!!! Updating master data.", c->chunkid,
						s->version, new_version);
				s->version = new_version;
			}
			if (s->version != c->version) {
				c->copy_has_wrong_version(s);
				return;
			}
			if (!s->is_todel() && todel) {
				s->mark_todel();
				c->updateStats();
			}
			if (s->is_todel() && !todel) {
				s->unmark_todel();
				c->updateStats();
			}
			return;
		}
	}
	const uint8_t state = (new_version == c->version) ? (todel ? TDVALID : VALID) : INVALID;
	c->addCopy(ptr, state, new_version, chunkType);
}

void chunk_damaged(void *ptr,uint64_t chunkid) {
	chunk *c;
	slist *s;
	c = chunk_find(chunkid);
	if (c==NULL) {
//		syslog(LOG_WARNING,"chunkserver has nonexistent chunk (%016" PRIX64 "), so create it for future deletion",chunkid);
		if (chunkid>=nextchunkid) {
			nextchunkid=chunkid+1;
		}
		c = chunk_new(chunkid);
		c->version = 0;
	}
	for (s=c->slisthead ; s ; s=s->next) {
		if (s->ptr==ptr) {
			c->invalidate_copy(s);
			c->needverincrease=1;
			return;
		}
	}
	c->addCopy(ptr, INVALID, 0, ChunkType::getStandardChunkType());
	c->needverincrease=1;
}

void chunk_lost(void *ptr,uint64_t chunkid) {
	chunk *c;
	slist **sptr,*s;
	c = chunk_find(chunkid);
	if (c==NULL) {
		return;
	}
	sptr=&(c->slisthead);
	while ((s=*sptr)) {
		if (s->ptr==ptr) {
			c->unlink_copy(s, sptr);
			c->needverincrease=1;
		} else {
			sptr = &(s->next);
		}
	}
}

void chunk_server_disconnected(void *ptr) {
	chunk *c;
	slist *s,**st;
	uint32_t i;
	for (i=0 ; i<HASHSIZE ; i++) {
		for (c=chunkhash[i] ; c ; c=c->next ) {
			st = &(c->slisthead);
			while (*st) {
				s = *st;
				if (s->ptr == ptr) {
					c->unlink_copy(s, st);
					c->needverincrease=1;
				} else {
					st = &(s->next);
				}
			}
			if (c->operation!=NONE) {
				bool any_copy_busy = false;
				uint8_t valid_copies = 0;
				for (s=c->slisthead ; s ; s=s->next) {
					any_copy_busy |= s->is_busy();
					valid_copies += s->is_valid() ? 1 : 0;
				}
				if (any_copy_busy) {
					c->interrupted = 1;
				} else {
					if (valid_copies > 0) {
						chunk_emergency_increase_version(c);
					} else {
						matoclserv_chunk_status(c->chunkid,ERROR_NOTDONE);
						c->operation=NONE;
					}
				}
			}
		}
	}
	fs_cs_disconnected();
}

void chunk_got_delete_status(void *ptr, uint64_t chunkId, ChunkType chunkType, uint8_t status) {
	chunk *c;
	slist *s,**st;
	c = chunk_find(chunkId);
	if (c==NULL) {
		return ;
	}
	st = &(c->slisthead);
	while (*st) {
		s = *st;
		if (s->ptr == ptr && s->chunkType == chunkType) {
			if (s->valid!=DEL) {
				syslog(LOG_WARNING,"got unexpected delete status");
			}
			c->unlink_copy(s, st);
		} else {
			st = &(s->next);
		}
	}
	if (status!=0) {
		return ;
	}
}

void chunk_got_replicate_status(void *ptr, uint64_t chunkId, uint32_t chunkVersion,
		ChunkType chunkType, uint8_t status) {
	slist *s;
	chunk *c = chunk_find(chunkId);
	if (c == NULL || status != 0) {
		return;
	}

	for (s = c->slisthead; s; s = s->next) {
		if (s->chunkType == chunkType && s->ptr == ptr) {
			syslog(LOG_WARNING,
					"got replication status from server which had had that chunk before (chunk:%016"
					PRIX64 "_%08" PRIX32 ")", chunkId, chunkVersion);
			if (s->valid == VALID && chunkVersion != c->version) {
				s->version = chunkVersion;
				c->copy_has_wrong_version(s);
			}
			return;
		}
	}
	const uint8_t state = (c->is_locked() || chunkVersion != c->version) ? INVALID : VALID;
	c->addCopy(ptr, state, chunkVersion, chunkType);
}

void chunk_operation_status(chunk *c, ChunkType chunkType, uint8_t status,void *ptr) {
	slist *s;
	uint8_t valid_copies = 0;
	bool any_copy_busy = false;
	for (s=c->slisthead ; s ; s=s->next) {
		if (s->ptr == ptr && s->chunkType == chunkType) {
			if (status!=0) {
				c->interrupted = 1;	// increase version after finish, just in case
				c->invalidate_copy(s);
			} else {
				if (s->is_busy()) {
					s->unmark_busy();
				}
			}
		}
		any_copy_busy |= s->is_busy();
		valid_copies += s->is_valid() ? 1 : 0;
	}
	if (!any_copy_busy) {
		if (valid_copies > 0) {
			if (c->interrupted) {
				chunk_emergency_increase_version(c);
			} else {
				matoclserv_chunk_status(c->chunkid,STATUS_OK);
				c->operation=NONE;
				c->needverincrease = 0;
			}
		} else {
			matoclserv_chunk_status(c->chunkid,ERROR_NOTDONE);
			c->operation=NONE;
		}
	}
}

void chunk_got_chunkop_status(void *ptr,uint64_t chunkid,uint8_t status) {
	chunk *c;
	c = chunk_find(chunkid);
	if (c==NULL) {
		return ;
	}
	chunk_operation_status(c, ChunkType::getStandardChunkType(), status, ptr);
}

void chunk_got_create_status(void *ptr,uint64_t chunkId, ChunkType chunkType, uint8_t status) {
	chunk *c;
	c = chunk_find(chunkId);
	if (c==NULL) {
		return ;
	}
	chunk_operation_status(c, chunkType, status, ptr);
}

void chunk_got_duplicate_status(void *ptr,uint64_t chunkid,uint8_t status) {
	chunk *c;
	c = chunk_find(chunkid);
	if (c==NULL) {
		return ;
	}
	chunk_operation_status(c, ChunkType::getStandardChunkType(), status, ptr);
}

void chunk_got_setversion_status(void *ptr, uint64_t chunkId, ChunkType chunkType, uint8_t status) {
	chunk *c;
	c = chunk_find(chunkId);
	if (c==NULL) {
		return ;
	}
	chunk_operation_status(c, chunkType, status, ptr);
}

void chunk_got_truncate_status(void *ptr, uint64_t chunkid, ChunkType chunkType, uint8_t status) {
	chunk *c;
	c = chunk_find(chunkid);
	if (c==NULL) {
		return ;
	}
	chunk_operation_status(c, chunkType, status, ptr);
}

void chunk_got_duptrunc_status(void *ptr,uint64_t chunkid,uint8_t status) {
	chunk *c;
	c = chunk_find(chunkid);
	if (c==NULL) {
		return ;
	}
	chunk_operation_status(c, ChunkType::getStandardChunkType(), status, ptr);
}

/* ----------------------- */
/* JOBS (DELETE/REPLICATE) */
/* ----------------------- */

void chunk_store_info(uint8_t *buff) {
	put32bit(&buff,chunksinfo_loopstart);
	put32bit(&buff,chunksinfo_loopend);
	put32bit(&buff,chunksinfo.done.del_invalid);
	put32bit(&buff,chunksinfo.notdone.del_invalid);
	put32bit(&buff,chunksinfo.done.del_unused);
	put32bit(&buff,chunksinfo.notdone.del_unused);
	put32bit(&buff,chunksinfo.done.del_diskclean);
	put32bit(&buff,chunksinfo.notdone.del_diskclean);
	put32bit(&buff,chunksinfo.done.del_overgoal);
	put32bit(&buff,chunksinfo.notdone.del_overgoal);
	put32bit(&buff,chunksinfo.done.copy_undergoal);
	put32bit(&buff,chunksinfo.notdone.copy_undergoal);
	put32bit(&buff,chunksinfo.copy_rebalance);
}

//jobs state: jobshpos

class ChunkWorker {
public:
	ChunkWorker();
	void doEveryLoopTasks();
	void doEverySecondTasks();
	void doChunkJobs(chunk *c, uint16_t serverCount, double minUsage, double maxUsage);

private:
	bool tryReplication(chunk *c, ChunkType type, void *destinationServer);

	uint16_t serverCount_;
	loop_info inforec_;
	uint32_t deleteNotDone_;
	uint32_t deleteDone_;
	uint32_t prevToDeleteCount_;
	uint32_t deleteLoopCount_;
};

ChunkWorker::ChunkWorker()
		: serverCount_(0),
		  deleteNotDone_(0),
		  deleteDone_(0),
		  prevToDeleteCount_(0),
		  deleteLoopCount_(0) {
	memset(&inforec_,0,sizeof(loop_info));
}

void ChunkWorker::doEveryLoopTasks() {
	deleteLoopCount_++;
	if (deleteLoopCount_ >= 16) {
		uint32_t toDeleteCount = deleteDone_ + deleteNotDone_;
		deleteLoopCount_ = 0;
		if ((deleteNotDone_ > deleteDone_) && (toDeleteCount > prevToDeleteCount_)) {
			TmpMaxDelFrac *= 1.5;
			if (TmpMaxDelFrac>MaxDelHardLimit) {
				syslog(LOG_NOTICE,"DEL_LIMIT hard limit (%" PRIu32 " per server) reached",MaxDelHardLimit);
				TmpMaxDelFrac=MaxDelHardLimit;
			}
			TmpMaxDel = TmpMaxDelFrac;
			syslog(LOG_NOTICE,"DEL_LIMIT temporary increased to: %" PRIu32 " per server",TmpMaxDel);
		}
		if ((toDeleteCount < prevToDeleteCount_) && (TmpMaxDelFrac > MaxDelSoftLimit)) {
			TmpMaxDelFrac /= 1.5;
			if (TmpMaxDelFrac<MaxDelSoftLimit) {
				syslog(LOG_NOTICE,"DEL_LIMIT back to soft limit (%" PRIu32 " per server)",MaxDelSoftLimit);
				TmpMaxDelFrac = MaxDelSoftLimit;
			}
			TmpMaxDel = TmpMaxDelFrac;
			syslog(LOG_NOTICE,"DEL_LIMIT decreased back to: %" PRIu32 " per server",TmpMaxDel);
		}
		prevToDeleteCount_ = toDeleteCount;
		deleteNotDone_ = 0;
		deleteDone_ = 0;
	}
	chunksinfo = inforec_;
	memset(&inforec_,0,sizeof(inforec_));
	chunksinfo_loopstart = chunksinfo_loopend;
	chunksinfo_loopend = main_time();
}

void ChunkWorker::doEverySecondTasks() {
	serverCount_ = 0;
}

static bool chunkPresentOnServer(chunk *c, void *server) {
	for (slist *s = c->slisthead ; s ; s = s->next) {
		if (s->ptr == server) {
			return true;
		}
	}
	return false;
}

bool ChunkWorker::tryReplication(chunk *c, ChunkType chunkTypeToRecover, void *destinationServer) {
	// NOTE: we don't allow replicating xor chunks from pre-1.6.28 chunkservers
	const uint32_t newServerVersion = lizardfsVersion(1, 6, 28);
	std::vector<void*> standardSources;
	std::vector<void*> newServerSources;
	ChunkCopiesCalculator newSourcesCalculator(c->goal);

	for (slist *s = c->slisthead ; s ; s = s->next) {
		if (s->is_valid() && !s->is_busy()) {
			if (matocsserv_get_version(s->ptr) >= newServerVersion) {
				newServerSources.push_back(s->ptr);
				newSourcesCalculator.addPart(s->chunkType);
			}
			if (s->chunkType.isStandardChunkType()) {
				standardSources.push_back(s->ptr);
			}
		}
	}

	if (newSourcesCalculator.isRecoveryPossible() &&
			matocsserv_get_version(destinationServer) >= newServerVersion) {
		// new replication possible - use it
		matocsserv_send_liz_replicatechunk(destinationServer, c->chunkid, c->version,
				chunkTypeToRecover, newServerSources,
				newSourcesCalculator.availableParts());
	} else if (chunkTypeToRecover.isStandardChunkType() && !standardSources.empty()) {
		// fall back to legacy replication
		matocsserv_send_replicatechunk(destinationServer, c->chunkid, c->version,
				standardSources[rndu32_ranged(standardSources.size())]);
	} else {
		// no replication possible
		return false;
	}
	stats_replications++;
	c->lockid = 0;             // remove stale lock
	c->needverincrease = 1;
	return true;
}

void ChunkWorker::doChunkJobs(chunk *c, uint16_t serverCount, double minUsage, double maxUsage) {
	slist *s;
	static void* ptrs[65535];
	static uint32_t min,max;

	// step 0. Update chunk's statistics
	// Just in case if somewhere is a bug and updateStats was not called
	c->updateStats();

	// step 1. calculate number of valid and invalid copies
	uint32_t vc, tdc, ivc, bc, tdb, dc;
	vc = tdc = ivc = bc = tdb = dc = 0;
	for (s = c->slisthead ; s ; s = s->next) {
		switch (s->valid) {
		case INVALID:
			ivc++;
			break;
		case TDVALID:
			tdc++;
			break;
		case VALID:
			vc++;
			break;
		case TDBUSY:
			tdb++;
			break;
		case BUSY:
			bc++;
			break;
		case DEL:
			dc++;
			break;
		}
	}

	// step 2. check number of copies
	if (tdc+vc+tdb+bc==0 && ivc>0 && c->fcount>0/* c->flisthead */) {
		syslog(LOG_WARNING,"chunk %016" PRIX64 " has only invalid copies (%" PRIu32 ") - please repair it manually",c->chunkid,ivc);
		for (s=c->slisthead ; s ; s=s->next) {
			syslog(LOG_NOTICE,"chunk %016" PRIX64 "_%08" PRIX32 " - invalid copy on (%s - ver:%08" PRIX32 ")",c->chunkid,c->version,matocsserv_getstrip(s->ptr),s->version);
		}
		return ;
	}

	// step 3. delete invalid copies
	for (s=c->slisthead ; s ; s=s->next) {
		if (matocsserv_deletion_counter(s->ptr)<TmpMaxDel) {
			if (!s->is_valid()) {
				if (s->valid==DEL) {
					syslog(LOG_WARNING,"chunk hasn't been deleted since previous loop - retry");
				}
				s->valid = DEL;
				stats_deletions++;
				matocsserv_send_deletechunk(s->ptr, c->chunkid, 0, s->chunkType);
				inforec_.done.del_invalid++;
				deleteDone_++;
				dc++;
				ivc--;
			}
		} else {
			if (s->valid==INVALID) {
				inforec_.notdone.del_invalid++;
				deleteNotDone_++;
			}
		}
	}

	// step 4. return if chunk is during some operation
	if (c->operation!=NONE || (c->is_locked())) {
		return ;
	}

	// step 5. check busy count
	if ((bc+tdb)>0) {
		syslog(LOG_WARNING,"chunk %016" PRIX64 " has unexpected BUSY copies",c->chunkid);
		return ;
	}

	// step 6. delete unused chunk
	if (c->fcount == 0) {
		for (s=c->slisthead ; s ; s=s->next) {
			if (matocsserv_deletion_counter(s->ptr)<TmpMaxDel) {
				if (s->is_valid() && !s->is_busy()) {
					c->delete_copy(s);
					c->needverincrease=1;
					stats_deletions++;
					matocsserv_send_deletechunk(s->ptr, c->chunkid, c->version, s->chunkType);
					inforec_.done.del_unused++;
					deleteDone_++;
				}
			} else {
				if (s->valid==VALID || s->valid==TDVALID) {
					inforec_.notdone.del_unused++;
					deleteNotDone_++;
				}
			}
		}
		return ;
	}

	// step 7a. if chunk needs replication, do it before removing any copies
	if (c->needsReplication()) {
		std::vector<ChunkType> toRecover = c->makeRegularCopiesCalculator().getPartsToRecover();
		if (jobsnorepbefore >= main_time() || c->isLost() || toRecover.empty()) {
			inforec_.notdone.copy_undergoal++;
			return;
		}
		const ChunkType chunkTypeToRecover = toRecover.front();
		// get list of chunkservers which can be written to
		std::vector<void*> possibleDestinations;
		matocsserv_getservers_lessrepl(possibleDestinations, MaxWriteRepl);
		// find the first one which does not contain any copy of the chunk
		// TODO(msulikowski) if we want to support converting between different goals
		// (eg. xor2 -> 3) on installations with small number of chunkservers this condition
		// has to be loosen
		uint32_t minServerVersion = 0;
		if (!chunkTypeToRecover.isStandardChunkType()) {
			minServerVersion = lizardfsVersion(1, 6, 28);
		}
		void *destination = nullptr;
		for (void* server : possibleDestinations) {
			if (matocsserv_get_version(server) < minServerVersion) {
				continue;
			}
			if (chunkPresentOnServer(c, server)) {
				continue;
			}
			destination = server;
			break;
		}
		if (destination == nullptr) {
			inforec_.notdone.copy_undergoal++;
			return;
		}
		if (tryReplication(c, chunkTypeToRecover, destination)) {
			inforec_.done.copy_undergoal++;
		} else {
			inforec_.notdone.copy_undergoal++;
		}
		return;
	}

	// step 7b. if chunk has too many copies then delete some of them
	if (c->needsDeletion()) {
		std::vector<ChunkType> toRemove = c->makeRegularCopiesCalculator().getPartsToRemove();
		if (serverCount_ == 0) {
			serverCount_ = matocsserv_getservers_ordered(ptrs,AcceptableDifference/2.0,&min,&max);
		}
		uint32_t copiesRemoved = 0;
		for (uint32_t i = 0; i < serverCount_ && !toRemove.empty(); ++i) {
			for (s = c->slisthead; s && s->ptr != ptrs[serverCount_ - 1 - i]; s = s->next) {}
			if (s && s->valid == VALID) {
				auto it = std::find(toRemove.begin(), toRemove.end(), s->chunkType);
				if (it == toRemove.end()) {
					continue;
				}
				if (matocsserv_deletion_counter(s->ptr) < TmpMaxDel) {
					c->delete_copy(s);
					c->needverincrease=1;
					stats_deletions++;
					matocsserv_send_deletechunk(s->ptr, c->chunkid, 0, s->chunkType);
					toRemove.erase(it);
					copiesRemoved++;
					vc--;
					dc++;
				} else {
					break;
				}
			}
		}
		inforec_.done.del_overgoal += copiesRemoved;
		deleteDone_ += copiesRemoved;
		inforec_.notdone.del_overgoal += (toRemove.size() - copiesRemoved);
		deleteNotDone_ += (toRemove.size() - copiesRemoved);
		return;
	}

	// step 7c. if chunk has one copy on each server and some of them have status TODEL then delete one of it
	bool hasXorCopies = false;
	for (s=c->slisthead ; s ; s=s->next) {
		if (!s->chunkType.isStandardChunkType()) {
			hasXorCopies = true;
		}
	}
	if (isOrdinaryGoal(c->goal)
			&& !hasXorCopies
			&& vc + tdc >= serverCount
			&& vc < c->goal
			&& tdc > 0
			&& vc + tdc > 1) {
		uint8_t prevdone;
		prevdone = 0;
		for (s=c->slisthead ; s && prevdone==0 ; s=s->next) {
			if (s->valid==TDVALID) {
				if (matocsserv_deletion_counter(s->ptr)<TmpMaxDel) {
					c->delete_copy(s);
					c->needverincrease=1;
					stats_deletions++;
					matocsserv_send_deletechunk(s->ptr, c->chunkid, 0, s->chunkType);
					inforec_.done.del_diskclean++;
					tdc--;
					dc++;
					prevdone = 1;
				} else {
					inforec_.notdone.del_diskclean++;
				}
			}
		}
		return;
	}

	if (chunksinfo.notdone.copy_undergoal > 0 && chunksinfo.done.copy_undergoal > 0) {
		return;
	}

	// step 9. if there is too big difference between chunkservers then make copy of chunk from server with biggest disk usage on server with lowest disk usage
	if (c->goal >= vc && vc + tdc>0 && (maxUsage - minUsage) > AcceptableDifference) {
		if (serverCount_==0) {
			serverCount_ = matocsserv_getservers_ordered(ptrs,AcceptableDifference/2.0,&min,&max);
		}
		if (min>0 || max>0) {
			ChunkType chunkType = ChunkType::getStandardChunkType();
			void *srcserv=NULL;
			void *dstserv=NULL;
			if (max>0) {
				for (uint32_t i=0 ; i<max && srcserv==NULL ; i++) {
					if (matocsserv_replication_read_counter(ptrs[serverCount_-1-i])<MaxReadRepl) {
						for (s=c->slisthead ; s && s->ptr!=ptrs[serverCount_-1-i] ; s=s->next ) {}
						if (s && (s->valid==VALID || s->valid==TDVALID)) {
							srcserv = s->ptr;
							chunkType = s->chunkType;
						}
					}
				}
			} else {
				for (uint32_t i=0 ; i<(serverCount_-min) && srcserv==NULL ; i++) {
					if (matocsserv_replication_read_counter(ptrs[serverCount_-1-i])<MaxReadRepl) {
						for (s=c->slisthead ; s && s->ptr!=ptrs[serverCount_-1-i] ; s=s->next ) {}
						if (s && (s->valid==VALID || s->valid==TDVALID)) {
							srcserv = s->ptr;
							chunkType = s->chunkType;
						}
					}
				}
			}
			if (srcserv!=NULL) {
				if (min>0) {
					for (uint32_t i=0 ; i<min && dstserv==NULL ; i++) {
						if (matocsserv_replication_write_counter(ptrs[i])<MaxWriteRepl) {
							if (!chunkPresentOnServer(c, ptrs[i])) {
								dstserv=ptrs[i];
							}
						}
					}
				} else {
					for (uint32_t i=0 ; i<serverCount_-max && dstserv==NULL ; i++) {
						if (matocsserv_replication_write_counter(ptrs[i])<MaxWriteRepl) {
							if (!chunkPresentOnServer(c, ptrs[i])) {
								dstserv=ptrs[i];
							}
						}
					}
				}
				if (dstserv!=NULL) {
					if (tryReplication(c, chunkType, dstserv)) {
						inforec_.copy_rebalance++;
					}
				}
			}
		}
	}
}

void chunk_jobs_main(void) {
	static ChunkWorker chunkWorker;
	uint32_t i,l,lc,r;
	uint16_t usableServerCount, totalServerCount;
	static uint16_t lastTotalServerCount = 0;
	static uint16_t maxTotalServerCount = 0;
	double minUsage, maxUsage;
	chunk *c,**cp;

	if (starttime + ReplicationsDelayInit > main_time()) {
		return;
	}

	matocsserv_usagedifference(&minUsage, &maxUsage, &usableServerCount, &totalServerCount);

	if (totalServerCount < lastTotalServerCount) {		// servers disconnected
		jobsnorepbefore = main_time() + ReplicationsDelayDisconnect;
	} else if (totalServerCount > lastTotalServerCount) {	// servers connected
		if (totalServerCount >= maxTotalServerCount) {
			maxTotalServerCount = totalServerCount;
			jobsnorepbefore = main_time();
		}
	} else if (totalServerCount < maxTotalServerCount && (uint32_t)main_time() > jobsnorepbefore) {
		maxTotalServerCount = totalServerCount;
	}
	lastTotalServerCount = totalServerCount;

	if (minUsage > maxUsage) {
		return;
	}

	chunkWorker.doEverySecondTasks();
	lc = 0;
	for (i=0 ; i<HashSteps && lc<HashCPS ; i++) {
		if (jobshpos==0) {
			chunkWorker.doEveryLoopTasks();
		}
		// delete unused chunks from structures
		l=0;
		cp = &(chunkhash[jobshpos]);
		while ((c=*cp)!=NULL) {
			if (c->fcount==0 && c->slisthead==NULL) {
				*cp = (c->next);
				chunk_delete(c);
			} else {
				cp = &(c->next);
				l++;
				lc++;
			}
		}
		if (l>0) {
			r = rndu32_ranged(l);
			l=0;
		// do jobs on rest of them
			for (c=chunkhash[jobshpos] ; c ; c=c->next) {
				if (l>=r) {
					chunkWorker.doChunkJobs(c, usableServerCount, minUsage, maxUsage);
				}
				l++;
			}
			l=0;
			for (c=chunkhash[jobshpos] ; l<r && c ; c=c->next) {
				chunkWorker.doChunkJobs(c, usableServerCount, minUsage, maxUsage);
				l++;
			}
		}
		jobshpos+=123;	// if HASHSIZE is any power of 2 then any odd number is good here
		jobshpos%=HASHSIZE;
	}
}

#endif

constexpr uint32_t kSerializedChunkSizeNoLockId = 16;
constexpr uint32_t kSerializedChunkSizeWithLockId = 20;
#define CHUNKCNT 1000

#ifdef METARESTORE

void chunk_dump(void) {
	chunk *c;
	uint32_t i,lockedto,now;
	now = time(NULL);

	for (i=0 ; i<HASHSIZE ; i++) {
		for (c=chunkhash[i] ; c ; c=c->next) {
			lockedto = c->lockedto;
			if (lockedto<now) {
				lockedto = 0;
			}
			printf("*|i:%016" PRIX64 "|v:%08" PRIX32 "|g:%" PRIu8 "|t:%10" PRIu32 "\n",c->chunkid,c->version,c->goal,lockedto);
		}
	}
}

#endif

int chunk_load(FILE *fd, bool loadLockIds) {
	uint8_t hdr[8];
	const uint8_t *ptr;
	int32_t r;
	chunk *c;
// chunkdata
	uint64_t chunkid;
	uint32_t version,lockedto;

	if (fread(hdr,1,8,fd)!=8) {
		return -1;
	}
	ptr = hdr;
	nextchunkid = get64bit(&ptr);
	int32_t serializedChunkSize = (loadLockIds
			? kSerializedChunkSizeWithLockId : kSerializedChunkSizeNoLockId);
	std::vector<uint8_t> loadbuff(serializedChunkSize);
	for (;;) {
		r = fread(loadbuff.data(), 1, serializedChunkSize, fd);
		if (r != serializedChunkSize) {
			return -1;
		}
		ptr = loadbuff.data();
		chunkid = get64bit(&ptr);
		if (chunkid>0) {
			c = chunk_new(chunkid);
			c->version = get32bit(&ptr);
			c->lockedto = get32bit(&ptr);
			if (loadLockIds) {
				c->lockid = get32bit(&ptr);
			}
		} else {
			version = get32bit(&ptr);
			lockedto = get32bit(&ptr);
			if (version==0 && lockedto==0) {
				return 0;
			} else {
				return -1;
			}
		}
	}
	return 0;	// unreachable
}

void chunk_store(FILE *fd) {
	uint8_t hdr[8];
	uint8_t storebuff[kSerializedChunkSizeWithLockId * CHUNKCNT];
	uint8_t *ptr;
	uint32_t i,j;
	chunk *c;
// chunkdata
	uint64_t chunkid;
	uint32_t version;
	uint32_t lockedto,lockid,now;
#ifndef METARESTORE
	now = main_time();
#else
	now = time(NULL);
#endif
	ptr = hdr;
	put64bit(&ptr,nextchunkid);
	if (fwrite(hdr,1,8,fd)!=(size_t)8) {
		return;
	}
	j=0;
	ptr = storebuff;
	for (i=0 ; i<HASHSIZE ; i++) {
		for (c=chunkhash[i] ; c ; c=c->next) {
			chunkid = c->chunkid;
			put64bit(&ptr,chunkid);
			version = c->version;
			put32bit(&ptr,version);
			lockedto = c->lockedto;
			lockid = c->lockid;
			if (lockedto<now) {
				lockedto = 0;
				lockid = 0;
			}
			put32bit(&ptr,lockedto);
			put32bit(&ptr,lockid);
			j++;
			if (j==CHUNKCNT) {
				size_t writtenBlockSize = kSerializedChunkSizeWithLockId * CHUNKCNT;
				if (fwrite(storebuff, 1, writtenBlockSize, fd) != writtenBlockSize) {
					return;
				}
				j=0;
				ptr = storebuff;
			}
		}
	}
	memset(ptr, 0, kSerializedChunkSizeWithLockId);
	j++;
	size_t writtenBlockSize = kSerializedChunkSizeWithLockId * j;
	if (fwrite(storebuff, 1, writtenBlockSize, fd) != writtenBlockSize) {
		return;
	}
}

void chunk_term(void) {
#ifndef METARESTORE
# ifdef USE_SLIST_BUCKETS
	slist_bucket *sb,*sbn;
# else
	slist *sl,*sln;
# endif
# if 0
# ifdef USE_FLIST_BUCKETS
	flist_bucket *fb,*fbn;
# else
	flist *fl,*fln;
# endif
# endif
# ifdef USE_CHUNK_BUCKETS
	chunk_bucket *cb,*cbn;
# endif
# if !defined(USE_SLIST_BUCKETS) || !defined(USE_FLIST_BUCKETS) || !defined(USE_CHUNK_BUCKETS)
	uint32_t i;
	chunk *ch,*chn;
# endif
#else
# ifdef USE_CHUNK_BUCKETS
	chunk_bucket *cb,*cbn;
# else
	uint32_t i;
	chunk *ch,*chn;
# endif
#endif

#ifndef METARESTORE
# ifdef USE_SLIST_BUCKETS
	for (sb = sbhead ; sb ; sb = sbn) {
		sbn = sb->next;
		delete sb;
	}
# else
	for (i=0 ; i<HASHSIZE ; i++) {
		for (ch = chunkhash[i] ; ch ; ch = ch->next) {
			for (sl = ch->slisthead ; sl ; sl = sln) {
				sln = sl->next;
				delete sl;
			}
		}
	}
# endif
#endif
#ifdef USE_CHUNK_BUCKETS
	for (cb = cbhead ; cb ; cb = cbn) {
		cbn = cb->next;
		free(cb);
	}
#else
	for (i=0 ; i<HASHSIZE ; i++) {
		for (ch = chunkhash[i] ; ch ; ch = chn) {
			chn = ch->next;
			free(ch);
		}
	}
#endif
}

void chunk_newfs(void) {
	nextchunkid = 1;
}

#ifndef METARESTORE
void chunk_reload(void) {
	uint32_t repl;
	uint32_t looptime;

	ReplicationsDelayInit = cfg_getuint32("REPLICATIONS_DELAY_INIT",300);
	ReplicationsDelayDisconnect = cfg_getuint32("REPLICATIONS_DELAY_DISCONNECT",3600);

	uint32_t disableChunksDel = cfg_getuint32("DISABLE_CHUNKS_DEL", 0);
	if (disableChunksDel) {
		MaxDelSoftLimit = MaxDelHardLimit = 0;
	} else {
		uint32_t oldMaxDelSoftLimit = MaxDelSoftLimit;
		uint32_t oldMaxDelHardLimit = MaxDelHardLimit;

		MaxDelSoftLimit = cfg_getuint32("CHUNKS_SOFT_DEL_LIMIT",10);
		if (cfg_isdefined("CHUNKS_HARD_DEL_LIMIT")) {
			MaxDelHardLimit = cfg_getuint32("CHUNKS_HARD_DEL_LIMIT",25);
			if (MaxDelHardLimit<MaxDelSoftLimit) {
				MaxDelSoftLimit = MaxDelHardLimit;
				syslog(LOG_WARNING,"CHUNKS_SOFT_DEL_LIMIT is greater than CHUNKS_HARD_DEL_LIMIT - using CHUNKS_HARD_DEL_LIMIT for both");
			}
		} else {
			MaxDelHardLimit = 3 * MaxDelSoftLimit;
		}
		if (MaxDelSoftLimit==0) {
			MaxDelSoftLimit = oldMaxDelSoftLimit;
			MaxDelHardLimit = oldMaxDelHardLimit;
		}
	}
	if (TmpMaxDelFrac<MaxDelSoftLimit) {
		TmpMaxDelFrac = MaxDelSoftLimit;
	}
	if (TmpMaxDelFrac>MaxDelHardLimit) {
		TmpMaxDelFrac = MaxDelHardLimit;
	}
	if (TmpMaxDel<MaxDelSoftLimit) {
		TmpMaxDel = MaxDelSoftLimit;
	}
	if (TmpMaxDel>MaxDelHardLimit) {
		TmpMaxDel = MaxDelHardLimit;
	}

	repl = cfg_getuint32("CHUNKS_WRITE_REP_LIMIT",2);
	if (repl>0) {
		MaxWriteRepl = repl;
	}


	repl = cfg_getuint32("CHUNKS_READ_REP_LIMIT",10);
	if (repl>0) {
		MaxReadRepl = repl;
	}

	if (cfg_isdefined("CHUNKS_LOOP_TIME")) {
		looptime = cfg_getuint32("CHUNKS_LOOP_TIME",300);
		if (looptime < MINLOOPTIME) {
			syslog(LOG_NOTICE,"CHUNKS_LOOP_TIME value too low (%" PRIu32 ") increased to %u",looptime,MINLOOPTIME);
			looptime = MINLOOPTIME;
		}
		if (looptime > MAXLOOPTIME) {
			syslog(LOG_NOTICE,"CHUNKS_LOOP_TIME value too high (%" PRIu32 ") decreased to %u",looptime,MAXLOOPTIME);
			looptime = MAXLOOPTIME;
		}
		HashSteps = 1+((HASHSIZE)/looptime);
		HashCPS = 0xFFFFFFFF;
	} else {
		looptime = cfg_getuint32("CHUNKS_LOOP_MIN_TIME",300);
		if (looptime < MINLOOPTIME) {
			syslog(LOG_NOTICE,"CHUNKS_LOOP_MIN_TIME value too low (%" PRIu32 ") increased to %u",looptime,MINLOOPTIME);
			looptime = MINLOOPTIME;
		}
		if (looptime > MAXLOOPTIME) {
			syslog(LOG_NOTICE,"CHUNKS_LOOP_MIN_TIME value too high (%" PRIu32 ") decreased to %u",looptime,MAXLOOPTIME);
			looptime = MAXLOOPTIME;
		}
		HashSteps = 1+((HASHSIZE)/looptime);
		HashCPS = cfg_getuint32("CHUNKS_LOOP_MAX_CPS",100000);
		if (HashCPS < MINCPS) {
			syslog(LOG_NOTICE,"CHUNKS_LOOP_MAX_CPS value too low (%" PRIu32 ") increased to %u",HashCPS,MINCPS);
			HashCPS = MINCPS;
		}
		if (HashCPS > MAXCPS) {
			syslog(LOG_NOTICE,"CHUNKS_LOOP_MAX_CPS value too high (%" PRIu32 ") decreased to %u",HashCPS,MAXCPS);
			HashCPS = MAXCPS;
		}
	}

	AcceptableDifference = cfg_getdouble("ACCEPTABLE_DIFFERENCE",0.1);
	if (AcceptableDifference<0.001) {
		AcceptableDifference = 0.001;
	}
	if (AcceptableDifference>10.0) {
		AcceptableDifference = 10.0;
	}
}
#endif

int chunk_strinit(void) {
#ifndef METARESTORE
	uint32_t looptime;

	uint32_t disableChunksDel = cfg_getuint32("DISABLE_CHUNKS_DEL", 0);
	ReplicationsDelayInit = cfg_getuint32("REPLICATIONS_DELAY_INIT",300);
	ReplicationsDelayDisconnect = cfg_getuint32("REPLICATIONS_DELAY_DISCONNECT",3600);
	if (disableChunksDel) {
		MaxDelHardLimit = MaxDelSoftLimit = 0;
	} else {
		MaxDelSoftLimit = cfg_getuint32("CHUNKS_SOFT_DEL_LIMIT",10);
		if (cfg_isdefined("CHUNKS_HARD_DEL_LIMIT")) {
			MaxDelHardLimit = cfg_getuint32("CHUNKS_HARD_DEL_LIMIT",25);
			if (MaxDelHardLimit<MaxDelSoftLimit) {
				MaxDelSoftLimit = MaxDelHardLimit;
				fprintf(stderr,"CHUNKS_SOFT_DEL_LIMIT is greater than CHUNKS_HARD_DEL_LIMIT - using CHUNKS_HARD_DEL_LIMIT for both\n");
			}
		} else {
			MaxDelHardLimit = 3 * MaxDelSoftLimit;
		}
		if (MaxDelSoftLimit == 0) {
			fprintf(stderr,"delete limit is zero !!!\n");
			return -1;
		}
	}
	TmpMaxDelFrac = MaxDelSoftLimit;
	TmpMaxDel = MaxDelSoftLimit;
	MaxWriteRepl = cfg_getuint32("CHUNKS_WRITE_REP_LIMIT",2);
	MaxReadRepl = cfg_getuint32("CHUNKS_READ_REP_LIMIT",10);
	if (MaxReadRepl==0) {
		fprintf(stderr,"read replication limit is zero !!!\n");
		return -1;
	}
	if (MaxWriteRepl==0) {
		fprintf(stderr,"write replication limit is zero !!!\n");
		return -1;
	}
	if (cfg_isdefined("CHUNKS_LOOP_TIME")) {
		fprintf(stderr,"Defining loop time by CHUNKS_LOOP_TIME option is deprecated - use CHUNKS_LOOP_MAX_CPS and CHUNKS_LOOP_MIN_TIME\n");
		looptime = cfg_getuint32("CHUNKS_LOOP_TIME",300);
		if (looptime < MINLOOPTIME) {
			fprintf(stderr,"CHUNKS_LOOP_TIME value too low (%" PRIu32 ") increased to %u\n",looptime,MINLOOPTIME);
			looptime = MINLOOPTIME;
		}
		if (looptime > MAXLOOPTIME) {
			fprintf(stderr,"CHUNKS_LOOP_TIME value too high (%" PRIu32 ") decreased to %u\n",looptime,MAXLOOPTIME);
			looptime = MAXLOOPTIME;
		}
		HashSteps = 1+((HASHSIZE)/looptime);
		HashCPS = 0xFFFFFFFF;
	} else {
		looptime = cfg_getuint32("CHUNKS_LOOP_MIN_TIME",300);
		if (looptime < MINLOOPTIME) {
			fprintf(stderr,"CHUNKS_LOOP_MIN_TIME value too low (%" PRIu32 ") increased to %u\n",looptime,MINLOOPTIME);
			looptime = MINLOOPTIME;
		}
		if (looptime > MAXLOOPTIME) {
			fprintf(stderr,"CHUNKS_LOOP_MIN_TIME value too high (%" PRIu32 ") decreased to %u\n",looptime,MAXLOOPTIME);
			looptime = MAXLOOPTIME;
		}
		HashSteps = 1+((HASHSIZE)/looptime);
		HashCPS = cfg_getuint32("CHUNKS_LOOP_MAX_CPS",100000);
		if (HashCPS < MINCPS) {
			fprintf(stderr,"CHUNKS_LOOP_MAX_CPS value too low (%" PRIu32 ") increased to %u\n",HashCPS,MINCPS);
			HashCPS = MINCPS;
		}
		if (HashCPS > MAXCPS) {
			fprintf(stderr,"CHUNKS_LOOP_MAX_CPS value too high (%" PRIu32 ") decreased to %u\n",HashCPS,MAXCPS);
			HashCPS = MAXCPS;
		}
	}
	AcceptableDifference = cfg_getdouble("ACCEPTABLE_DIFFERENCE",0.1);
	if (AcceptableDifference<0.001) {
		AcceptableDifference = 0.001;
	}
	if (AcceptableDifference>10.0) {
		AcceptableDifference = 10.0;
	}
#endif
	for (uint32_t i=0 ; i<HASHSIZE ; i++) {
		chunkhash[i]=NULL;
	}
#ifndef METARESTORE
	jobshpos = 0;
	jobsrebalancecount = 0;
	starttime = main_time();
	jobsnorepbefore = starttime+ReplicationsDelayInit;
	main_reloadregister(chunk_reload);
	main_timeregister(TIMEMODE_RUN_LATE,1,0,chunk_jobs_main);
#endif
	return 1;
}
