/* Copyright (c) 2013-2015 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "lockstep.h"

#include "gba/gba.h"
#include "gba/io.h"
#include "gba/video.h"

#define LOCKSTEP_INCREMENT 3000

static bool GBASIOLockstepNodeInit(struct GBASIODriver* driver);
static void GBASIOLockstepNodeDeinit(struct GBASIODriver* driver);
static bool GBASIOLockstepNodeLoad(struct GBASIODriver* driver);
static bool GBASIOLockstepNodeUnload(struct GBASIODriver* driver);
static uint16_t GBASIOLockstepNodeMultiWriteRegister(struct GBASIODriver* driver, uint32_t address, uint16_t value);
static uint16_t GBASIOLockstepNodeNormalWriteRegister(struct GBASIODriver* driver, uint32_t address, uint16_t value);
static int32_t GBASIOLockstepNodeProcessEvents(struct GBASIODriver* driver, int32_t cycles);
static int32_t GBASIOLockstepNodeProcessEvents(struct GBASIODriver* driver, int32_t cycles);

void GBASIOLockstepInit(struct GBASIOLockstep* lockstep) {
	lockstep->players[0] = 0;
	lockstep->players[1] = 0;
	lockstep->players[2] = 0;
	lockstep->players[3] = 0;
	lockstep->multiRecv[0] = 0xFFFF;
	lockstep->multiRecv[1] = 0xFFFF;
	lockstep->multiRecv[2] = 0xFFFF;
	lockstep->multiRecv[3] = 0xFFFF;
	lockstep->attached = 0;
	lockstep->attachedMulti = 0;
	lockstep->transferActive = 0;
#ifndef NDEBUG
	lockstep->transferId = 0;
#endif
}

void GBASIOLockstepDeinit(struct GBASIOLockstep* lockstep) {
	UNUSED(lockstep);
}

void GBASIOLockstepNodeCreate(struct GBASIOLockstepNode* node) {
	node->d.init = GBASIOLockstepNodeInit;
	node->d.deinit = GBASIOLockstepNodeDeinit;
	node->d.load = GBASIOLockstepNodeLoad;
	node->d.unload = GBASIOLockstepNodeUnload;
	node->d.writeRegister = 0;
	node->d.processEvents = GBASIOLockstepNodeProcessEvents;
}

bool GBASIOLockstepAttachNode(struct GBASIOLockstep* lockstep, struct GBASIOLockstepNode* node) {
	if (lockstep->attached == MAX_GBAS) {
		return false;
	}
	lockstep->players[lockstep->attached] = node;
	node->p = lockstep;
	node->id = lockstep->attached;
	++lockstep->attached;
	return true;
}

void GBASIOLockstepDetachNode(struct GBASIOLockstep* lockstep, struct GBASIOLockstepNode* node) {
	if (lockstep->attached == 0) {
		return;
	}
	int i;
	for (i = 0; i < lockstep->attached; ++i) {
		if (lockstep->players[i] != node) {
			continue;
		}
		for (++i; i < lockstep->attached; ++i) {
			lockstep->players[i - 1] = lockstep->players[i];
			lockstep->players[i - 1]->id = i - 1;
		}
		--lockstep->attached;
		break;
	}
}

bool GBASIOLockstepNodeInit(struct GBASIODriver* driver) {
	struct GBASIOLockstepNode* node = (struct GBASIOLockstepNode*) driver;
	node->d.p->multiplayerControl.slave = node->id > 0;
	mLOG(GBA_SIO, DEBUG, "Lockstep %i: Node init", node->id);
	return true;
}

void GBASIOLockstepNodeDeinit(struct GBASIODriver* driver) {
	UNUSED(driver);
}

bool GBASIOLockstepNodeLoad(struct GBASIODriver* driver) {
	struct GBASIOLockstepNode* node = (struct GBASIOLockstepNode*) driver;
	node->nextEvent = 0;
	node->eventDiff = 0;
	node->mode = driver->p->mode;
	switch (node->mode) {
	case SIO_MULTI:
		node->d.writeRegister = GBASIOLockstepNodeMultiWriteRegister;
		node->d.p->rcnt |= 3;
		++node->p->attachedMulti;
		node->d.p->multiplayerControl.ready = node->p->attachedMulti == node->p->attached;
		if (node->id) {
			node->d.p->rcnt |= 4;
			node->d.p->multiplayerControl.slave = 1;
		}
		break;
	case SIO_NORMAL_32:
		node->d.writeRegister = GBASIOLockstepNodeNormalWriteRegister;
		break;
	default:
		break;
	}
#ifndef NDEBUG
	node->phase = node->p->transferActive;
	node->transferId = node->p->transferId;
#endif
	return true;
}

bool GBASIOLockstepNodeUnload(struct GBASIODriver* driver) {
	struct GBASIOLockstepNode* node = (struct GBASIOLockstepNode*) driver;
	node->mode = driver->p->mode;
	switch (node->mode) {
	case SIO_MULTI:
		--node->p->attachedMulti;
		break;
	default:
		break;
	}
	node->p->unload(node->p, node->id);
	return true;
}

static uint16_t GBASIOLockstepNodeMultiWriteRegister(struct GBASIODriver* driver, uint32_t address, uint16_t value) {
	struct GBASIOLockstepNode* node = (struct GBASIOLockstepNode*) driver;
	if (address == REG_SIOCNT) {
		mLOG(GBA_SIO, DEBUG, "Lockstep %i: SIOCNT <- %04x", node->id, value);
		if (value & 0x0080 && node->p->transferActive == TRANSFER_IDLE) {
			if (!node->id && node->d.p->multiplayerControl.ready) {
				mLOG(GBA_SIO, DEBUG, "Lockstep %i: Transfer initiated", node->id);
				node->p->transferActive = TRANSFER_STARTING;
				node->p->transferCycles = GBASIOCyclesPerTransfer[node->d.p->multiplayerControl.baud][node->p->attached - 1];
				node->nextEvent = 0;
			} else {
				value &= ~0x0080;
			}
		}
		value &= 0xFF83;
		value |= driver->p->siocnt & 0x00FC;
	} else if (address == REG_SIOMLT_SEND) {
		mLOG(GBA_SIO, DEBUG, "Lockstep %i: SIOMLT_SEND <- %04x", node->id, value);
	}
	return value;
}

static void _finishTransfer(struct GBASIOLockstepNode* node) {
	if (node->transferFinished) {
		return;
	}
	struct GBASIO* sio = node->d.p;
	switch (node->mode) {
	case SIO_MULTI:
		sio->p->memory.io[REG_SIOMULTI0 >> 1] = node->p->multiRecv[0];
		sio->p->memory.io[REG_SIOMULTI1 >> 1] = node->p->multiRecv[1];
		sio->p->memory.io[REG_SIOMULTI2 >> 1] = node->p->multiRecv[2];
		sio->p->memory.io[REG_SIOMULTI3 >> 1] = node->p->multiRecv[3];
		sio->rcnt |= 1;
		sio->multiplayerControl.busy = 0;
		sio->multiplayerControl.id = node->id;
		if (sio->multiplayerControl.irq) {
			GBARaiseIRQ(sio->p, IRQ_SIO);
		}
		break;
	case SIO_NORMAL_8:
		// TODO
		sio->normalControl.start = 0;
		if (node->id) {
			sio->normalControl.si = node->p->players[node->id - 1]->d.p->normalControl.idleSo;
			node->d.p->p->memory.io[REG_SIODATA8 >> 1] = node->p->normalRecv[node->id - 1] & 0xFF;
		} else {
			node->d.p->p->memory.io[REG_SIODATA8 >> 1] = 0xFFFF;
		}
		if (sio->multiplayerControl.irq) {
			GBARaiseIRQ(sio->p, IRQ_SIO);
		}
		break;
	case SIO_NORMAL_32:
		// TODO
		sio->normalControl.start = 0;
		if (node->id) {
			sio->normalControl.si = node->p->players[node->id - 1]->d.p->normalControl.idleSo;
			node->d.p->p->memory.io[REG_SIODATA32_LO >> 1] = node->p->normalRecv[node->id - 1];
			node->d.p->p->memory.io[REG_SIODATA32_HI >> 1] |= node->p->normalRecv[node->id - 1] >> 16;
		} else {
			node->d.p->p->memory.io[REG_SIODATA32_LO >> 1] = 0xFFFF;
			node->d.p->p->memory.io[REG_SIODATA32_HI >> 1] = 0xFFFF;
		}
		if (sio->multiplayerControl.irq) {
			GBARaiseIRQ(sio->p, IRQ_SIO);
		}
		break;
	default:
		break;
	}
	node->transferFinished = true;
#ifndef NDEBUG
	++node->transferId;
#endif
}

static int32_t _masterUpdate(struct GBASIOLockstepNode* node) {
	bool needsToWait = false;
	int i;
	switch (node->p->transferActive) {
	case TRANSFER_IDLE:
		// If the master hasn't initiated a transfer, it can keep going.
		node->nextEvent += LOCKSTEP_INCREMENT;
		node->d.p->multiplayerControl.ready = node->p->attachedMulti == node->p->attached;
		break;
	case TRANSFER_STARTING:
		// Start the transfer, but wait for the other GBAs to catch up
		node->transferFinished = false;
		node->p->multiRecv[0] = 0xFFFF;
		node->p->multiRecv[1] = 0xFFFF;
		node->p->multiRecv[2] = 0xFFFF;
		node->p->multiRecv[3] = 0xFFFF;
		needsToWait = true;
		ATOMIC_STORE(node->p->transferActive, TRANSFER_STARTED);
		node->nextEvent += 512;
		break;
	case TRANSFER_STARTED:
		// All the other GBAs have caught up and are sleeping, we can all continue now
		node->p->multiRecv[0] = node->d.p->p->memory.io[REG_SIOMLT_SEND >> 1];
		node->nextEvent += 512;
		ATOMIC_STORE(node->p->transferActive, TRANSFER_FINISHING);
		break;
	case TRANSFER_FINISHING:
		// Finish the transfer
		// We need to make sure the other GBAs catch up so they don't get behind
		node->nextEvent += node->p->transferCycles - 1024; // Split the cycles to avoid waiting too long
#ifndef NDEBUG
		ATOMIC_ADD(node->p->transferId, 1);
#endif
		needsToWait = true;
		ATOMIC_STORE(node->p->transferActive, TRANSFER_FINISHED);
		break;
	case TRANSFER_FINISHED:
		// Everything's settled. We're done.
		_finishTransfer(node);
		node->nextEvent += LOCKSTEP_INCREMENT;
		ATOMIC_STORE(node->p->transferActive, TRANSFER_IDLE);
		break;
	}
	int mask = 0;
	for (i = 1; i < node->p->attached; ++i) {
		if (node->p->players[i]->mode == node->mode) {
			mask |= 1 << i;
		}
	}
	if (mask) {
		if (needsToWait) {
			if (!node->p->wait(node->p, mask)) {
				abort();
			}
		} else {
			node->p->signal(node->p, mask);
		}
	}
	// Tell the other GBAs they can continue up to where we were
	node->p->addCycles(node->p, 0, node->eventDiff);
#ifndef NDEBUG
	node->phase = node->p->transferActive;
#endif
	if (needsToWait) {
		return 0;
	}
	return node->nextEvent;
}

static uint32_t _slaveUpdate(struct GBASIOLockstepNode* node) {
	node->d.p->multiplayerControl.ready = node->p->attachedMulti == node->p->attached;
	bool signal = false;
	switch (node->p->transferActive) {
	case TRANSFER_IDLE:
		if (!node->d.p->multiplayerControl.ready) {
			node->p->addCycles(node->p, node->id, LOCKSTEP_INCREMENT);
		}
		break;
	case TRANSFER_STARTING:
	case TRANSFER_FINISHING:
		break;
	case TRANSFER_STARTED:
		node->transferFinished = false;
		switch (node->mode) {
		case SIO_MULTI:
			node->d.p->rcnt &= ~1;
			node->p->multiRecv[node->id] = node->d.p->p->memory.io[REG_SIOMLT_SEND >> 1];
			node->d.p->p->memory.io[REG_SIOMULTI0 >> 1] = 0xFFFF;
			node->d.p->p->memory.io[REG_SIOMULTI1 >> 1] = 0xFFFF;
			node->d.p->p->memory.io[REG_SIOMULTI2 >> 1] = 0xFFFF;
			node->d.p->p->memory.io[REG_SIOMULTI3 >> 1] = 0xFFFF;
			node->d.p->multiplayerControl.busy = 1;
			break;
		case SIO_NORMAL_8:
			node->p->multiRecv[node->id] = 0xFFFF;
			node->p->normalRecv[node->id] = node->d.p->p->memory.io[REG_SIODATA8 >> 1] & 0xFF;
			break;
		case SIO_NORMAL_32:
			node->p->multiRecv[node->id] = 0xFFFF;
			node->p->normalRecv[node->id] = node->d.p->p->memory.io[REG_SIODATA32_LO >> 1];
			node->p->normalRecv[node->id] |= node->d.p->p->memory.io[REG_SIODATA32_HI >> 1] << 16;
			break;
		default:
			node->p->multiRecv[node->id] = 0xFFFF;
			break;
		}
		signal = true;
		break;
	case TRANSFER_FINISHED:
		_finishTransfer(node);
		signal = true;
		break;
	}
#ifndef NDEBUG
	node->phase = node->p->transferActive;
#endif
	if (signal) {
		node->p->signal(node->p, 1 << node->id);
	}
	return 0;
}

static int32_t GBASIOLockstepNodeProcessEvents(struct GBASIODriver* driver, int32_t cycles) {
	struct GBASIOLockstepNode* node = (struct GBASIOLockstepNode*) driver;
	if (node->p->attached < 2) {
		return INT_MAX;
	}
	node->eventDiff += cycles;
	node->nextEvent -= cycles;
	if (node->nextEvent <= 0) {
		if (!node->id) {
			cycles = _masterUpdate(node);
		} else {
			cycles = _slaveUpdate(node);
			node->nextEvent += node->p->useCycles(node->p, node->id, node->eventDiff);
		}
		node->eventDiff = 0;
	} else {
		cycles = node->nextEvent;
	}
	if (cycles < 0) {
		return 0;
	}
	return cycles;
}

static uint16_t GBASIOLockstepNodeNormalWriteRegister(struct GBASIODriver* driver, uint32_t address, uint16_t value) {
	struct GBASIOLockstepNode* node = (struct GBASIOLockstepNode*) driver;
	if (address == REG_SIOCNT) {
		mLOG(GBA_SIO, DEBUG, "Lockstep %i: SIOCNT <- %04x", node->id, value);
		value &= 0xFF8B;
		if (!node->id) {
			driver->p->normalControl.si = 1;
		}
		if (value & 0x0080 && !node->id) {
			// Internal shift clock
			if (value & 1) {
				node->p->transferActive = TRANSFER_STARTING;
			}
			// Frequency
			if (value & 2) {
				node->p->transferCycles = GBA_ARM7TDMI_FREQUENCY / 1024;
			} else {
				node->p->transferCycles = GBA_ARM7TDMI_FREQUENCY / 8192;
			}
		}
	} else if (address == REG_SIODATA32_LO) {
		mLOG(GBA_SIO, DEBUG, "Lockstep %i: SIODATA32_LO <- %04x", node->id, value);
	} else if (address == REG_SIODATA32_HI) {
		mLOG(GBA_SIO, DEBUG, "Lockstep %i: SIODATA32_HI <- %04x", node->id, value);
	}
	return value;
}
