/* Copyright (c) 2013-2015 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "cheats.h"

#include "gba/cheats/gameshark.h"
#include "gba/cheats/parv3.h"
#include "gba/gba.h"
#include "util/string.h"

#define MAX_LINE_LENGTH 128

static void _addBreakpoint(struct mCheatDevice* device, struct GBACheatSet* cheats) {
	if (!device->p || !cheats->hook) {
		return;
	}
	++cheats->hook->reentries;
	if (cheats->hook->reentries > 1) {
		return;
	}
	// TODO: Put back hooks
}

static void _removeBreakpoint(struct mCheatDevice* device, struct GBACheatSet* cheats) {
	if (!device->p || !cheats->hook) {
		return;
	}
	--cheats->hook->reentries;
	if (cheats->hook->reentries > 0) {
		return;
	}
	// TODO: Put back hooks
}

static void _patchROM(struct mCheatDevice* device, struct GBACheatSet* cheats) {
	if (!device->p) {
		return;
	}
	int i;
	for (i = 0; i < MAX_ROM_PATCHES; ++i) {
		if (!cheats->romPatches[i].exists || cheats->romPatches[i].applied) {
			continue;
		}
		GBAPatch16(device->p->cpu, cheats->romPatches[i].address, cheats->romPatches[i].newValue, &cheats->romPatches[i].oldValue);
		cheats->romPatches[i].applied = true;
	}
}

static void _unpatchROM(struct mCheatDevice* device, struct GBACheatSet* cheats) {
	if (!device->p) {
		return;
	}
	int i;
	for (i = 0; i < MAX_ROM_PATCHES; ++i) {
		if (!cheats->romPatches[i].exists || !cheats->romPatches[i].applied) {
			continue;
		}
		GBAPatch16(device->p->cpu, cheats->romPatches[i].address, cheats->romPatches[i].oldValue, 0);
		cheats->romPatches[i].applied = false;
	}
}

static void GBACheatSetDeinit(struct mCheatSet* set);
static void GBACheatAddSet(struct mCheatSet* cheats, struct mCheatDevice* device);
static void GBACheatRemoveSet(struct mCheatSet* cheats, struct mCheatDevice* device);
static void GBACheatRefresh(struct mCheatSet* cheats, struct mCheatDevice* device);
static void GBACheatSetCopyProperties(struct mCheatSet* set, struct mCheatSet* oldSet);
static void GBACheatParseDirectives(struct mCheatSet*, const struct StringList* directives);
static void GBACheatDumpDirectives(struct mCheatSet*, struct StringList* directives);
static bool GBACheatAddLine(struct mCheatSet*, const char* line, int type);

static struct mCheatSet* GBACheatSetCreate(struct mCheatDevice* device, const char* name) {
	UNUSED(device);
	struct GBACheatSet* set = malloc(sizeof(*set));
	mCheatSetInit(&set->d, name);
	set->incompleteCheat = -1;
	set->incompletePatch = 0;
	set->currentBlock = -1;
	set->gsaVersion = 0;
	set->cbRngState = 0;
	set->cbMaster = 0;
	set->remainingAddresses = 0;
	set->hook = NULL;

	set->d.deinit = GBACheatSetDeinit;
	set->d.add = GBACheatAddSet;
	set->d.remove = GBACheatRemoveSet;

	set->d.addLine = GBACheatAddLine;
	set->d.copyProperties = GBACheatSetCopyProperties;

	set->d.parseDirectives = GBACheatParseDirectives;
	set->d.dumpDirectives = GBACheatDumpDirectives;

	set->d.refresh = GBACheatRefresh;

	int i;
	for (i = 0; i < MAX_ROM_PATCHES; ++i) {
		set->romPatches[i].exists = false;
	}
	return &set->d;
}

struct mCheatDevice* GBACheatDeviceCreate(void) {
	struct mCheatDevice* device = malloc(sizeof(*device));
	mCheatDeviceCreate(device);
	device->createSet = GBACheatSetCreate;
	return device;
}

static void GBACheatSetDeinit(struct mCheatSet* set) {
	struct GBACheatSet* gbaset = (struct GBACheatSet*) set;
	if (gbaset->hook) {
		--gbaset->hook->refs;
		if (gbaset->hook->refs == 0) {
			free(gbaset->hook);
		}
	}
}

static void GBACheatAddSet(struct mCheatSet* cheats, struct mCheatDevice* device) {
	struct GBACheatSet* gbaset = (struct GBACheatSet*) cheats;
	_addBreakpoint(device, gbaset);
	_patchROM(device, gbaset);
}

static void GBACheatRemoveSet(struct mCheatSet* cheats, struct mCheatDevice* device) {
	struct GBACheatSet* gbaset = (struct GBACheatSet*) cheats;
	_unpatchROM(device, gbaset);
	_removeBreakpoint(device, gbaset);
}

static bool GBACheatAddAutodetect(struct GBACheatSet* set, uint32_t op1, uint32_t op2) {
	uint32_t o1 = op1;
	uint32_t o2 = op2;
	char line[18] = "XXXXXXXX XXXXXXXX";
	snprintf(line, sizeof(line), "%08X %08X", op1, op2);

	switch (set->gsaVersion) {
	case 0:
		// Try to detect GameShark version
		GBACheatDecryptGameShark(&o1, &o2, GBACheatGameSharkSeeds);
		if ((o1 & 0xF0000000) == 0xF0000000 && !(o2 & 0xFFFFFCFE)) {
			GBACheatSetGameSharkVersion(set, 1);
			return GBACheatAddGameSharkRaw(set, o1, o2);
		}
		o1 = op1;
		o2 = op2;
		GBACheatDecryptGameShark(&o1, &o2, GBACheatProActionReplaySeeds);
		if ((o1 & 0xFE000000) == 0xC4000000 && !(o2 & 0xFFFF0000)) {
			GBACheatSetGameSharkVersion(set, 3);
			return GBACheatAddProActionReplayRaw(set, o1, o2);
		}
		break;
	case 1:
		GBACheatDecryptGameShark(&o1, &o2, set->gsaSeeds);
		return GBACheatAddGameSharkRaw(set, o1, o2);
	case 3:
		GBACheatDecryptGameShark(&o1, &o2, set->gsaSeeds);
		return GBACheatAddProActionReplayRaw(set, o1, o2);
	}
	return false;
}

bool GBACheatAddVBALine(struct GBACheatSet* cheats, const char* line) {
	uint32_t address;
	uint8_t op;
	uint32_t value = 0;
	int width = 0;
	const char* lineNext = hex32(line, &address);
	if (!lineNext) {
		return false;
	}
	if (lineNext[0] != ':') {
		return false;
	}
	++lineNext;
	while (width < 4) {
		lineNext = hex8(lineNext, &op);
		if (!lineNext) {
			break;
		}
		value <<= 8;
		value |= op;
		++width;
	}
	if (width == 0 || width == 3) {
		return false;
	}

	struct mCheat* cheat = mCheatListAppend(&cheats->d.list);
	cheat->address = address;
	cheat->operandOffset = 0;
	cheat->addressOffset = 0;
	cheat->repeat = 1;
	cheat->type = CHEAT_ASSIGN;
	cheat->width = width;
	cheat->operand = value;
	return true;
}

bool GBACheatAddLine(struct mCheatSet* set, const char* line, int type) {
	struct GBACheatSet* cheats = (struct GBACheatSet*) set;
	switch (type) {
	case GBA_CHEAT_AUTODETECT:
		break;
	case GBA_CHEAT_CODEBREAKER:
		return GBACheatAddCodeBreakerLine(cheats, line);
	case GBA_CHEAT_GAMESHARK:
		return GBACheatAddGameSharkLine(cheats, line);
	case GBA_CHEAT_PRO_ACTION_REPLAY:
		return GBACheatAddProActionReplayLine(cheats, line);
	case GBA_CHEAT_VBA:
		return GBACheatAddVBALine(cheats, line);
	default:
		return false;
	}

	uint32_t op1;
	uint16_t op2;
	uint16_t op3;
	const char* lineNext = hex32(line, &op1);
	if (!lineNext) {
		return false;
	}
	if (lineNext[0] == ':') {
		return GBACheatAddVBALine(cheats, line);
	}
	while (isspace((int) lineNext[0])) {
		++lineNext;
	}
	lineNext = hex16(lineNext, &op2);
	if (!lineNext) {
		return false;
	}
	if (!lineNext[0] || isspace((int) lineNext[0])) {
		return GBACheatAddCodeBreaker(cheats, op1, op2);
	}
	lineNext = hex16(lineNext, &op3);
	if (!lineNext) {
		return false;
	}
	uint32_t realOp2 = op2;
	realOp2 <<= 16;
	realOp2 |= op3;
	return GBACheatAddAutodetect(cheats, op1, realOp2);
}

static void GBACheatRefresh(struct mCheatSet* cheats, struct mCheatDevice* device) {
	struct GBACheatSet* gbaset = (struct GBACheatSet*) cheats;
	_patchROM(device, gbaset);
}

static void GBACheatSetCopyProperties(struct mCheatSet* set, struct mCheatSet* oldSet) {
	struct GBACheatSet* newSet = (struct GBACheatSet*) set;
	struct GBACheatSet* gbaset = (struct GBACheatSet*) oldSet;
	newSet->gsaVersion = gbaset->gsaVersion;
	memcpy(newSet->gsaSeeds, gbaset->gsaSeeds, sizeof(newSet->gsaSeeds));
	newSet->cbRngState = gbaset->cbRngState;
	newSet->cbMaster = gbaset->cbMaster;
	memcpy(newSet->cbSeeds, gbaset->cbSeeds, sizeof(newSet->cbSeeds));
	memcpy(newSet->cbTable, gbaset->cbTable, sizeof(newSet->cbTable));
	if (gbaset->hook) {
		if (newSet->hook) {
			--newSet->hook->refs;
			if (newSet->hook->refs == 0) {
				free(newSet->hook);
			}
		}
		newSet->hook = gbaset->hook;
		++newSet->hook->refs;
	}
}

static void GBACheatParseDirectives(struct mCheatSet* set, const struct StringList* directives) {
	struct GBACheatSet* cheats = (struct GBACheatSet*) set;
	size_t d;
	for (d = 0; d < StringListSize(directives); ++d) {
		const char* directive = *StringListGetConstPointer(directives, d);
		if (strcmp(directive, "GSAv1") == 0) {
			GBACheatSetGameSharkVersion(cheats, 1);
			continue;
		}
		if (strcmp(directive, "PARv3") == 0) {
			GBACheatSetGameSharkVersion(cheats, 3);
			continue;
		}
	}
}

static void GBACheatDumpDirectives(struct mCheatSet* set, struct StringList* directives) {
	struct GBACheatSet* cheats = (struct GBACheatSet*) set;

	// TODO: Check previous directives
	size_t d;
	for (d = 0; d < StringListSize(directives); ++d) {
		free(*StringListGetPointer(directives, d));
	}
	StringListClear(directives);

	char** directive;
	switch (cheats->gsaVersion) {
	case 1:
	case 2:
		directive = StringListAppend(directives);
		*directive = strdup("GSAv1");
		break;
	case 3:
	case 4:
		directive = StringListAppend(directives);
		*directive = strdup("PARv3");
		break;
	}
}
