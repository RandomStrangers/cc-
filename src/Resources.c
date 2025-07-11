#include "Resources.h"
#ifdef CC_BUILD_RESOURCES
#include "Funcs.h"
#include "String.h"
#include "Constants.h"
#include "Deflate.h"
#include "Stream.h"
#include "Platform.h"
#include "Launcher.h"
#include "Utils.h"
#include "Vorbis.h"
#include "Errors.h"
#include "Logger.h"
#include "LWeb.h"
#include "Http.h"
#include "Game.h"
#include "Audio.h"

/* Represents a set of assets/resources */
/* E.g. music set, sounds set, textures set */
struct AssetSet {
	/* Checks whether all assets in this asset set exist on disc */
	void (*CheckExistence)(void);
	/* Counts the number of size of missing assets that need to be downloaded */
	void (*CountMissing)(void);
	/* Begins asynchronously downloading the missing assets in this asset set */
	void (*DownloadAssets)(void);
	/* Returns the name for the associated request, or NULL otherwise */
	const char* (*GetRequestName)(int reqID);
	/* Checks if any assets have been downloaded, and processes them if so */
	void (*CheckStatus)(void);
	/* Resets state and frees and allocated memory */
	void (*ResetState)(void);
};

int Resources_MissingCount, Resources_MissingSize;
cc_bool Resources_MissingRequired;

union ResourceValue {
	cc_uint8* data;
	struct Bitmap bmp;
};
struct ResourceZipEntry {
	const char* filename;
	/* zip data */
	cc_uint32 type : 3;
	cc_uint32 size : 29;
	union ResourceValue value;
	cc_uint32 offset, crc32;
};
#define RESOURCE_TYPE_DATA  1
#define RESOURCE_TYPE_PNG   2
#define RESOURCE_TYPE_CONST 3
#define RESOURCE_TYPE_SOUND 4

static CC_NOINLINE cc_bool Fetcher_Get(int reqID, struct HttpRequest* item);
CC_NOINLINE static struct ResourceZipEntry* ZipEntries_Find(const cc_string* name);
static cc_result ZipEntry_ExtractData(struct ResourceZipEntry* e, struct Stream* data, struct ZipEntry* source);


/*########################################################################################################################*
*------------------------------------------------------Utility functions -------------------------------------------------*
*#########################################################################################################################*/
static void ZipFile_InspectEntries(const cc_string* path, Zip_SelectEntry selector) {
	struct Stream stream;
	cc_result res;
	struct ZipEntry entries[64];

	res = Stream_OpenFile(&stream, path);
	if (res == ReturnCode_FileNotFound) return;
	if (res) { Logger_SysWarn2(res, "opening", path); return; }

	res = Zip_Extract(&stream, selector, NULL, 
						entries, Array_Elems(entries));
	if (res) Logger_SysWarn2(res, "inspecting", path);

	/* No point logging error for closing readonly file */
	(void)stream.Close(&stream);
}

static cc_result ZipEntry_ExtractData(struct ResourceZipEntry* e, struct Stream* data, struct ZipEntry* source) {
	cc_uint32 size = source->UncompressedSize;
	e->value.data  = (cc_uint8*)Mem_TryAlloc(size, 1);
	e->size        = size;

	if (!e->value.data) return ERR_OUT_OF_MEMORY;
	return Stream_Read(data, e->value.data, size);
}


/*########################################################################################################################*
*------------------------------------------------------Zip entry writer---------------------------------------------------*
*#########################################################################################################################*/
static void GetCurrentZipDate(int* modTime, int* modDate) {
	struct cc_datetime now;
	DateTime_CurrentLocal(&now);

	*modTime = (now.second / 2) | (now.minute << 5) | (now.hour << 11);
	*modDate = (now.day) | (now.month << 5) | ((now.year - 1980) << 9);
}

static cc_result ZipWriter_LocalFile(struct Stream* s, struct ResourceZipEntry* e) {
	int filenameLen = String_Length(e->filename);
	cc_uint8 header[30 + STRING_SIZE];
	cc_result res;
	int modTime, modDate;

	GetCurrentZipDate(&modTime, &modDate);
	if ((res = s->Position(s, &e->offset))) return res;

	Stream_SetU32_LE(header + 0,  0x04034b50);  /* signature */
	Stream_SetU16_LE(header + 4,  20);          /* version needed */
	Stream_SetU16_LE(header + 6,  0);           /* bitflags */
	Stream_SetU16_LE(header + 8,  0);           /* compression method */
	Stream_SetU16_LE(header + 10, modTime);     /* last modified */
	Stream_SetU16_LE(header + 12, modDate);     /* last modified */
	
	Stream_SetU32_LE(header + 14, e->crc32);    /* CRC32 */
	Stream_SetU32_LE(header + 18, e->size);     /* Compressed size */
	Stream_SetU32_LE(header + 22, e->size);     /* Uncompressed size */
	 
	Stream_SetU16_LE(header + 26, filenameLen); /* name length */
	Stream_SetU16_LE(header + 28, 0);           /* extra field length */

	Mem_Copy(header + 30, e->filename, filenameLen);
	return Stream_Write(s, header, 30 + filenameLen);
}

static cc_result ZipWriter_CentralDir(struct Stream* s, struct ResourceZipEntry* e) {
	int filenameLen = String_Length(e->filename);
	cc_uint8 header[46 + STRING_SIZE];
	int modTime, modDate;
	GetCurrentZipDate(&modTime, &modDate);

	Stream_SetU32_LE(header + 0,  0x02014b50);  /* signature */
	Stream_SetU16_LE(header + 4,  20);          /* version */
	Stream_SetU16_LE(header + 6,  20);          /* version needed */
	Stream_SetU16_LE(header + 8,  0);           /* bitflags */
	Stream_SetU16_LE(header + 10, 0);           /* compression method */
	Stream_SetU16_LE(header + 12, modTime);     /* last modified */
	Stream_SetU16_LE(header + 14, modDate);     /* last modified */

	Stream_SetU32_LE(header + 16, e->crc32);    /* CRC32 */
	Stream_SetU32_LE(header + 20, e->size);     /* compressed size */
	Stream_SetU32_LE(header + 24, e->size);     /* uncompressed size */

	Stream_SetU16_LE(header + 28, filenameLen); /* name length */
	Stream_SetU16_LE(header + 30, 0);           /* extra field length */
	Stream_SetU16_LE(header + 32, 0);           /* file comment length */
	Stream_SetU16_LE(header + 34, 0);           /* disk number */
	Stream_SetU16_LE(header + 36, 0);           /* internal attributes */
	Stream_SetU32_LE(header + 38, 0);           /* external attributes */
	Stream_SetU32_LE(header + 42, e->offset);   /* local header offset */

	Mem_Copy(header + 46, e->filename, filenameLen);
	return Stream_Write(s, header, 46 + filenameLen);
}

static cc_result ZipWriter_EndOfCentralDir(struct Stream* s, int numEntries,
											cc_uint32 centralDirBeg, cc_uint32 centralDirEnd) {
	cc_uint8 header[22];

	Stream_SetU32_LE(header + 0,  0x06054b50); /* signature */
	Stream_SetU16_LE(header + 4,  0);          /* disk number */
	Stream_SetU16_LE(header + 6,  0);          /* disk number of start */
	Stream_SetU16_LE(header + 8,  numEntries); /* disk entries */
	Stream_SetU16_LE(header + 10, numEntries); /* total entries */
	Stream_SetU32_LE(header + 12, centralDirEnd - centralDirBeg);  /* central dir size */
	Stream_SetU32_LE(header + 16, centralDirBeg);                  /* central dir start */
	Stream_SetU16_LE(header + 20, 0);         /* comment length */
	return Stream_Write(s, header, 22);
}

static cc_result ZipWriter_FixupLocalFile(struct Stream* s, struct ResourceZipEntry* e) {
	int filenameLen = String_Length(e->filename);
	cc_uint8 tmp[2048];
	cc_uint32 dataBeg, dataEnd;
	cc_uint32 i, crc, toRead, read;
	cc_result res;

	dataBeg = e->offset + 30 + filenameLen;
	if ((res = s->Position(s, &dataEnd))) return res;
	e->size = dataEnd - dataBeg;

	/* work out the CRC 32 */
	crc = 0xffffffffUL;
	if ((res = s->Seek(s, dataBeg))) return res;

	for (; dataBeg < dataEnd; dataBeg += read) {
		toRead = dataEnd - dataBeg;
		toRead = min(toRead, sizeof(tmp));

		if ((res = s->Read(s, tmp, toRead, &read))) return res;
		if (!read) return ERR_END_OF_STREAM;

		for (i = 0; i < read; i++) {
			crc = Utils_Crc32Table[(crc ^ tmp[i]) & 0xFF] ^ (crc >> 8);
		}
	}
	e->crc32 = crc ^ 0xffffffffUL;

	/* then fixup the header */
	if ((res = s->Seek(s, e->offset)))     return res;
	if ((res = ZipWriter_LocalFile(s, e))) return res;
	return s->Seek(s, dataEnd);
}

static cc_result ZipWriter_WriteData(struct Stream* dst, struct ResourceZipEntry* e) {
	cc_uint8* data = e->value.data;
	cc_result res;
	e->crc32 = Utils_CRC32(data, e->size);

	if ((res = ZipWriter_LocalFile(dst, e))) return res;
	return Stream_Write(dst, data, e->size);
}

static cc_result ZipWriter_WritePng(struct Stream* dst, struct ResourceZipEntry* e) {
	struct Bitmap* src = &e->value.bmp;
	cc_result res;

	if ((res = ZipWriter_LocalFile(dst, e)))            return res;
	if ((res = Png_Encode(src, dst, NULL, true, NULL))) return res;
	return ZipWriter_FixupLocalFile(dst, e);
}

static cc_result SoundPatcher_Save(struct Stream* s, struct ResourceZipEntry* e);
static cc_result ZipWriter_WriteWav(struct Stream* dst, struct ResourceZipEntry* e) {
	cc_result res;
	
	if ((res = ZipWriter_LocalFile(dst, e))) return res;
	if ((res = SoundPatcher_Save(dst,   e))) return res;
	return ZipWriter_FixupLocalFile(dst, e);
}


/*########################################################################################################################*
*------------------------------------------------------Zip file writer----------------------------------------------------*
*#########################################################################################################################*/
static cc_result ZipFile_WriteEntries(struct Stream* s, struct ResourceZipEntry* entries, int numEntries) {
	struct ResourceZipEntry* e;
	cc_uint32 beg, end;
	int i;
	cc_result res;

	for (i = 0; i < numEntries; i++)
	{
		e = &entries[i];

		if (e->type == RESOURCE_TYPE_PNG) {
			if ((res = ZipWriter_WritePng(s,  e))) return res;
		} else if (e->type == RESOURCE_TYPE_SOUND) {
			if ((res = ZipWriter_WriteWav(s,  e))) return res;
		} else {
			if ((res = ZipWriter_WriteData(s, e))) return res;
		} 
	}
	
	if ((res = s->Position(s, &beg))) return res;
	for (i = 0; i < numEntries; i++)
	{
		if ((res = ZipWriter_CentralDir(s, &entries[i]))) return res;
	}

	if ((res = s->Position(s, &end))) return res;
	return ZipWriter_EndOfCentralDir(s, numEntries, beg, end);
}

static void ZipFile_Create(const cc_string* path, struct ResourceZipEntry* entries, int numEntries) {
	struct Stream s;
	cc_result res;

	res = Stream_CreateFile(&s, path);
	if (res) {
		Logger_SysWarn2(res, "creating", path); return;
	}
		
	res = ZipFile_WriteEntries(&s, entries, numEntries);
	if (res) Logger_SysWarn2(res, "making", path);

	res = s.Close(&s);
	if (res) Logger_SysWarn2(res, "closing", path);
}


#ifndef CC_BUILD_NOMUSIC
/*########################################################################################################################*
*---------------------------------------------------------Music assets----------------------------------------------------*
*#########################################################################################################################*/
static struct MusicAsset {
	const char* name;
	const char* hash;
	short size;
	cc_bool downloaded;
	int reqID;
} musicAssets[] = {
	{ "calm1.ogg", "50a59a4f56e4046701b758ddbb1c1587efa4cadf", 2472 },
	{ "calm2.ogg", "74da65c99aa578486efa7b69983d3533e14c0d6e", 1931 },
	{ "calm3.ogg", "14ae57a6bce3d4254daa8be2b098c2d99743cc3f", 2181 },
	{ "hal1.ogg",  "df1ff11b79757432c5c3f279e5ecde7b63ceda64", 1926 },
	{ "hal2.ogg",  "ceaaaa1d57dfdfbb0bd4da5ea39628b42897a687", 1714 },
	{ "hal3.ogg",  "dd85fb564e96ee2dbd4754f711ae9deb08a169f9", 1879 },
	{ "hal4.ogg",  "5e7d63e75c6e042f452bc5e151276911ef92fed8", 2499 }
};

static void MusicAssets_CheckExistence(void) {
	cc_string path; char pathBuffer[FILENAME_SIZE];
	cc_filepath str;
	int i;
	String_InitArray(path, pathBuffer);

	for (i = 0; i < Array_Elems(musicAssets); i++) 
	{
		path.length = 0;
		String_Format1(&path, "audio/%c", musicAssets[i].name);

		Platform_EncodePath(&str, &path);
		musicAssets[i].downloaded = File_Exists(&str);
	}
}

static void MusicAssets_CountMissing(void) {
	int i;
	for (i = 0; i < Array_Elems(musicAssets); i++) 
	{
		if (musicAssets[i].downloaded) continue;

		Resources_MissingSize += musicAssets[i].size;
		Resources_MissingCount++;
	}
}


/*########################################################################################################################*
*-----------------------------------------------------Music asset fetching -----------------------------------------------*
*#########################################################################################################################*/
CC_NOINLINE static int MusicAsset_Download(const char* hash) {
	cc_string url; char urlBuffer[URL_MAX_SIZE];

	String_InitArray(url, urlBuffer);
	String_Format3(&url, "https://resources.download.minecraft.net/%r%r/%c", 
					&hash[0], &hash[1], hash);
	return Http_AsyncGetData(&url, 0);
}

static void MusicAssets_DownloadAssets(void) {
	int i;
	for (i = 0; i < Array_Elems(musicAssets); i++) 
	{
		if (musicAssets[i].downloaded) continue;
		musicAssets[i].reqID = MusicAsset_Download(musicAssets[i].hash);
	}
}

static const char* MusicAssets_GetRequestName(int reqID) {
	int i;
	for (i = 0; i < Array_Elems(musicAssets); i++) 
	{
		if (reqID == musicAssets[i].reqID) return musicAssets[i].name;
	}
	return NULL;
}


/*########################################################################################################################*
*----------------------------------------------------Music asset processing ----------------------------------------------*
*#########################################################################################################################*/
static void MusicAsset_Save(const char* name, struct HttpRequest* req) {
	cc_string path; char pathBuffer[STRING_SIZE];
	cc_result res;

	String_InitArray(path, pathBuffer);
	String_Format1(&path, "audio/%c", name);

	res = Stream_WriteAllTo(&path, req->data, req->size);
	if (res) Logger_SysWarn(res, "saving music file");
}

static void MusicAsset_Check(struct MusicAsset* music) {
	struct HttpRequest item;
	if (!Fetcher_Get(music->reqID, &item)) return;

	music->downloaded = true;
	MusicAsset_Save(music->name, &item);
	HttpRequest_Free(&item);
}

static void MusicAssets_CheckStatus(void) {
	int i;
	for (i = 0; i < Array_Elems(musicAssets); i++) 
	{
		if (musicAssets[i].downloaded) continue;
		MusicAsset_Check(&musicAssets[i]);
	}
}

static void MusicAssets_ResetState(void) {
}

static const struct AssetSet mccMusicAssetSet = {
	MusicAssets_CheckExistence,
	MusicAssets_CountMissing,
	MusicAssets_DownloadAssets,
	MusicAssets_GetRequestName,
	MusicAssets_CheckStatus,
	MusicAssets_ResetState
};
#endif


#ifdef CC_BUILD_NOSOUNDS
static cc_result SoundPatcher_Save(struct Stream* s, struct ResourceZipEntry* e) { return ERR_NOT_SUPPORTED; }
#else
/*########################################################################################################################*
*-----------------------------------------------------Sound asset writing ------------------------------------------------*
*#########################################################################################################################*/
#define WAV_FourCC(a, b, c, d) (((cc_uint32)a << 24) | ((cc_uint32)b << 16) | ((cc_uint32)c << 8) | (cc_uint32)d)
#define WAV_HDR_SIZE 44

/* Fixes up the .WAV header after having written all samples */
static cc_result SoundPatcher_FixupHeader(struct Stream* s, struct VorbisState* ctx, cc_uint32 offset, cc_uint32 len) {
	cc_uint8 header[WAV_HDR_SIZE];
	cc_result res = s->Seek(s, offset);
	if (res) return res;

	Stream_SetU32_BE(header +  0, WAV_FourCC('R','I','F','F'));
	Stream_SetU32_LE(header +  4, len - 8);
	Stream_SetU32_BE(header +  8, WAV_FourCC('W','A','V','E'));
	Stream_SetU32_BE(header + 12, WAV_FourCC('f','m','t',' '));
	Stream_SetU32_LE(header + 16, 16); /* fmt chunk size */
	Stream_SetU16_LE(header + 20, 1);  /* PCM audio format */
	Stream_SetU16_LE(header + 22, ctx->channels);
	Stream_SetU32_LE(header + 24, ctx->sampleRate);

	Stream_SetU32_LE(header + 28, ctx->sampleRate * ctx->channels * 2); /* byte rate */
	Stream_SetU16_LE(header + 32, ctx->channels * 2);                   /* block align */
	Stream_SetU16_LE(header + 34, 16);                                  /* bits per sample */
	Stream_SetU32_BE(header + 36, WAV_FourCC('d','a','t','a'));
	Stream_SetU32_LE(header + 40, len - WAV_HDR_SIZE);

	return Stream_Write(s, header, WAV_HDR_SIZE);
}

/* Decodes all samples, then produces a .WAV file from them */
static cc_result SoundPatcher_WriteWav(struct Stream* s, struct VorbisState* ctx) {
	cc_int16* samples;
	cc_uint32 begOffset;
	cc_uint32 len = WAV_HDR_SIZE;
	cc_result res;
	int count;

	if ((res = s->Position(s, &begOffset))) return res;

	/* reuse context here for a temp garbage header */
	if ((res = Stream_Write(s, (const cc_uint8*)ctx, WAV_HDR_SIZE))) return res;
	if ((res = Vorbis_DecodeHeaders(ctx))) return res;

	samples = (cc_int16*)Mem_TryAlloc(ctx->blockSizes[1] * ctx->channels, 2);
	if (!samples) return ERR_OUT_OF_MEMORY;

	for (;;) {
		res = Vorbis_DecodeFrame(ctx);
		if (res == ERR_END_OF_STREAM) {
			/* reached end of samples, so done */
			res = SoundPatcher_FixupHeader(s, ctx, begOffset, len);
			break;
		}
		if (res) break;

		count = Vorbis_OutputFrame(ctx, samples);
		len  += count * 2;

#ifdef CC_BUILD_BIGENDIAN
		Utils_SwapEndian16(samples, count);
#endif
		res = Stream_Write(s, (cc_uint8*)samples, count * 2);
		if (res) break;
	}

	Mem_Free(samples);
	if (!res) res = s->Seek(s, begOffset + len);
	return res;
}

/* Converts an OGG sound to a WAV sound for faster decoding later */
static cc_result SoundPatcher_Save(struct Stream* s, struct ResourceZipEntry* e) {
	struct OggState* ogg    = NULL;
	struct VorbisState* ctx = NULL;
	struct Stream src;
	cc_result res;

	ogg = (struct OggState*)Mem_TryAlloc(1,    sizeof(struct OggState));
	if (!ogg) { res = ERR_OUT_OF_MEMORY; goto cleanup; }

	ctx = (struct VorbisState*)Mem_TryAlloc(1, sizeof(struct VorbisState));
	if (!ctx) { res = ERR_OUT_OF_MEMORY; goto cleanup; }

	Stream_ReadonlyMemory(&src, e->value.data, e->size);

	Ogg_Init(ogg, &src);
	Vorbis_Init(ctx);
	ctx->source = ogg;
	res = SoundPatcher_WriteWav(s, ctx);

cleanup:
	if (ctx) Vorbis_Free(ctx);
	Mem_Free(ctx);
	Mem_Free(ogg);
	return res;
}


/*########################################################################################################################*
*---------------------------------------------------------Sound assets----------------------------------------------------*
*#########################################################################################################################*/
static struct SoundAsset {
	const char* filename;
	const char* hash;
	int reqID, size;
	void* data;
} soundAssets[] = {
	{ "dig_cloth1.wav",  "5fd568d724ba7d53911b6cccf5636f859d2662e8" }, { "dig_cloth2.wav",  "56c1d0ac0de2265018b2c41cb571cc6631101484" },
	{ "dig_cloth3.wav",  "9c63f2a3681832dc32d206f6830360bfe94b5bfc" }, { "dig_cloth4.wav",  "55da1856e77cfd31a7e8c3d358e1f856c5583198" },
	{ "dig_grass1.wav",  "41cbf5dd08e951ad65883854e74d2e034929f572" }, { "dig_grass2.wav",  "86cb1bb0c45625b18e00a64098cd425a38f6d3f2" },
	{ "dig_grass3.wav",  "f7d7e5c7089c9b45fa5d1b31542eb455fad995db" }, { "dig_grass4.wav",  "c7b1005d4926f6a2e2387a41ab1fb48a72f18e98" },
	{ "dig_gravel1.wav", "e8b89f316f3e9989a87f6e6ff12db9abe0f8b09f" }, { "dig_gravel2.wav", "c3b3797d04cb9640e1d3a72d5e96edb410388fa3" },
	{ "dig_gravel3.wav", "48f7e1bb098abd36b9760cca27b9d4391a23de26" }, { "dig_gravel4.wav", "7bf3553a4fe41a0078f4988a13d6e1ed8663ef4c" },
	{ "dig_sand1.wav",   "9e59c3650c6c3fc0a475f1b753b2fcfef430bf81" }, { "dig_sand2.wav",   "0fa4234797f336ada4e3735e013e44d1099afe57" },
	{ "dig_sand3.wav",   "c75589cc0087069f387de127dd1499580498738e" }, { "dig_sand4.wav",   "37afa06f97d58767a1cd1382386db878be1532dd" },
	{ "dig_snow1.wav",   "e9bab7d3d15541f0aaa93fad31ad37fd07e03a6c" }, { "dig_snow2.wav",   "5887d10234c4f244ec5468080412f3e6ef9522f3" },
	{ "dig_snow3.wav",   "a4bc069321a96236fde04a3820664cc23b2ea619" }, { "dig_snow4.wav",   "e26fa3036cdab4c2264ceb19e1cd197a2a510227" },
	{ "dig_stone1.wav",  "4e094ed8dfa98656d8fec52a7d20c5ee6098b6ad" }, { "dig_stone2.wav",  "9c92f697142ae320584bf64c0d54381d59703528" },
	{ "dig_stone3.wav",  "8f23c02475d388b23e5faa680eafe6b991d7a9d4" }, { "dig_stone4.wav",  "363545a76277e5e47538b2dd3a0d6aa4f7a87d34" },
	{ "dig_wood1.wav",   "9bc2a84d0aa98113fc52609976fae8fc88ea6333" }, { "dig_wood2.wav",   "98102533e6085617a2962157b4f3658f59aea018" },
	{ "dig_wood3.wav",   "45b2aef7b5049e81b39b58f8d631563fadcc778b" }, { "dig_wood4.wav",   "dc66978374a46ab2b87db6472804185824868095" },
	{ "dig_glass1.wav",  "7274a2231ed4544a37e599b7b014e589e5377094" }, { "dig_glass2.wav",  "87c47bda3645c68f18a49e83cbf06e5302d087ff" },
	{ "dig_glass3.wav",  "ad7d770b7fff3b64121f75bd60cecfc4866d1cd6" },

	{ "step_cloth1.wav",  "5fd568d724ba7d53911b6cccf5636f859d2662e8" }, { "step_cloth2.wav",  "56c1d0ac0de2265018b2c41cb571cc6631101484" },
	{ "step_cloth3.wav",  "9c63f2a3681832dc32d206f6830360bfe94b5bfc" }, { "step_cloth4.wav",  "55da1856e77cfd31a7e8c3d358e1f856c5583198" },
	{ "step_grass1.wav",  "41cbf5dd08e951ad65883854e74d2e034929f572" }, { "step_grass2.wav",  "86cb1bb0c45625b18e00a64098cd425a38f6d3f2" },
	{ "step_grass3.wav",  "f7d7e5c7089c9b45fa5d1b31542eb455fad995db" }, { "step_grass4.wav",  "c7b1005d4926f6a2e2387a41ab1fb48a72f18e98" },
	{ "step_gravel1.wav", "e8b89f316f3e9989a87f6e6ff12db9abe0f8b09f" }, { "step_gravel2.wav", "c3b3797d04cb9640e1d3a72d5e96edb410388fa3" },
	{ "step_gravel3.wav", "48f7e1bb098abd36b9760cca27b9d4391a23de26" }, { "step_gravel4.wav", "7bf3553a4fe41a0078f4988a13d6e1ed8663ef4c" },
	{ "step_sand1.wav",   "9e59c3650c6c3fc0a475f1b753b2fcfef430bf81" }, { "step_sand2.wav",   "0fa4234797f336ada4e3735e013e44d1099afe57" },
	{ "step_sand3.wav",   "c75589cc0087069f387de127dd1499580498738e" }, { "step_sand4.wav",   "37afa06f97d58767a1cd1382386db878be1532dd" },
	{ "step_snow1.wav",   "e9bab7d3d15541f0aaa93fad31ad37fd07e03a6c" }, { "step_snow2.wav",   "5887d10234c4f244ec5468080412f3e6ef9522f3" },
	{ "step_snow3.wav",   "a4bc069321a96236fde04a3820664cc23b2ea619" }, { "step_snow4.wav",   "e26fa3036cdab4c2264ceb19e1cd197a2a510227" },
	{ "step_stone1.wav",  "4e094ed8dfa98656d8fec52a7d20c5ee6098b6ad" }, { "step_stone2.wav",  "9c92f697142ae320584bf64c0d54381d59703528" },
	{ "step_stone3.wav",  "8f23c02475d388b23e5faa680eafe6b991d7a9d4" }, { "step_stone4.wav",  "363545a76277e5e47538b2dd3a0d6aa4f7a87d34" },
	{ "step_wood1.wav",   "9bc2a84d0aa98113fc52609976fae8fc88ea6333" }, { "step_wood2.wav",   "98102533e6085617a2962157b4f3658f59aea018" },
	{ "step_wood3.wav",   "45b2aef7b5049e81b39b58f8d631563fadcc778b" }, { "step_wood4.wav",   "dc66978374a46ab2b87db6472804185824868095" }
};
static cc_bool allSoundsExist;

static void SoundAssets_ResetState(void) {
	int i;
	allSoundsExist = false;

	for (i = 0; i < Array_Elems(soundAssets); i++)
	{
		Mem_Free(soundAssets[i].data);
		soundAssets[i].data = NULL;
		soundAssets[i].size = 0;
	}
}


/*########################################################################################################################*
*-----------------------------------------------------Sound asset checking -----------------------------------------------*
*#########################################################################################################################*/
static int soundEntriesFound;

static struct SoundAsset* SoundAssest_Find(const cc_string* name) {
	struct SoundAsset* a;
	int i;

	for (i = 0; i < Array_Elems(soundAssets); i++) 
	{
		a = &soundAssets[i];
		if (String_CaselessEqualsConst(name, a->filename)) return a;
	}
	return NULL;
}

static cc_bool SoundAssets_CheckEntry(const cc_string* path) {
	cc_string name = *path;
	Utils_UNSAFE_GetFilename(&name);

	if (SoundAssest_Find(&name)) soundEntriesFound++;
	return false;
}

static void SoundAssets_CheckExistence(void) {
	soundEntriesFound = 0;
	ZipFile_InspectEntries(&Sounds_ZipPathMC, SoundAssets_CheckEntry);

	/* >= in case somehow have say "gui.png", "GUI.png" */
	allSoundsExist = soundEntriesFound >= Array_Elems(soundAssets);
}

static void SoundAssets_CountMissing(void) {
	if (allSoundsExist) return;

	Resources_MissingCount += Array_Elems(soundAssets);
	Resources_MissingSize  += 417;
}


/*########################################################################################################################*
*----------------------------------------------------Sound asset generation ----------------------------------------------*
*#########################################################################################################################*/
static void SoundAsset_CreateZip(void) {
	struct ResourceZipEntry entries[Array_Elems(soundAssets)];
	int i;

	for (i = 0; i < Array_Elems(soundAssets); i++)
	{
		entries[i].filename = soundAssets[i].filename;
		entries[i].type     = RESOURCE_TYPE_SOUND;

		entries[i].value.data = soundAssets[i].data;
		entries[i].size       = soundAssets[i].size;
	}

	ZipFile_Create(&Sounds_ZipPathMC, entries, Array_Elems(soundAssets));
	SoundAssets_ResetState();
}


/*########################################################################################################################*
*-----------------------------------------------------Sound asset fetching -----------------------------------------------*
*#########################################################################################################################*/
#define SoundAsset_Download(hash) MusicAsset_Download(hash)

static void SoundAssets_DownloadAssets(void) {
	int i;
	for (i = 0; i < Array_Elems(soundAssets); i++)
	{
		if (allSoundsExist) continue;
		soundAssets[i].reqID = SoundAsset_Download(soundAssets[i].hash);
	}
}

static const char* SoundAssets_GetRequestName(int reqID) {
	int i;
	for (i = 0; i < Array_Elems(soundAssets); i++) 
	{
		if (reqID == soundAssets[i].reqID) return soundAssets[i].filename;
	}
	return NULL;
}

static void SoundAsset_Check(struct SoundAsset* sound, int i) {
	struct HttpRequest item;
	if (!Fetcher_Get(sound->reqID, &item)) return;

	sound->data = item.data;
	sound->size = item.size;
	item.data   = NULL;
	HttpRequest_Free(&item);

	if (i == Array_Elems(soundAssets) - 1)
		SoundAsset_CreateZip();
}

static void SoundAssets_CheckStatus(void) {
	int i;
	for (i = 0; i < Array_Elems(soundAssets); i++)
	{
		SoundAsset_Check(&soundAssets[i], i);
	}
}

static const struct AssetSet mccSoundAssetSet = {
	SoundAssets_CheckExistence,
	SoundAssets_CountMissing,
	SoundAssets_DownloadAssets,
	SoundAssets_GetRequestName,
	SoundAssets_CheckStatus,
	SoundAssets_ResetState
};
#endif


/*########################################################################################################################*
*------------------------------------------------------CC texture assets--------------------------------------------------*
*#########################################################################################################################*/
static const cc_string ccTexPack = String_FromConst("texpacks/classicube.zip");
static cc_bool ccTexturesExist, ccTexturesDownloaded;
static int ccTexturesReqID;

static void CCTextures_CheckExistence(void) {
	cc_filepath path;
	Platform_EncodePath(&path, &ccTexPack);
	
	ccTexturesExist = File_Exists(&path);
}

static void CCTextures_CountMissing(void) {
	if (ccTexturesExist) return;

	Resources_MissingCount++;
	Resources_MissingSize += 83;
	Resources_MissingRequired = true;
}


/*########################################################################################################################*
*--------------------------------------------------CC texture assets fetching --------------------------------------------*
*#########################################################################################################################*/
static void CCTextures_DownloadAssets(void) {
	static cc_string url = String_FromConst(RESOURCE_SERVER "/default.zip");
	if (ccTexturesExist) return;

	ccTexturesReqID = Http_AsyncGetData(&url, 0);
}

static const char* CCTextures_GetRequestName(int reqID) {
	return reqID == ccTexturesReqID ? "ClassiCube textures" : NULL;
}


/*########################################################################################################################*
*-------------------------------------------------CC texture assets processing -------------------------------------------*
*#########################################################################################################################*/
/* Android needs the touch.png */
/* TODO: Unify both android and desktop platforms to both just extract from default.zip */
static cc_bool CCTextures_SelectEntry(const cc_string* path) {
	return String_CaselessEqualsConst(path, "touch.png");
}

static cc_result CCTextures_ProcessEntry(const cc_string* path, struct Stream* data, struct ZipEntry* source) {
	struct ResourceZipEntry* e = ZipEntries_Find(path);
	if (!e) return 0; /* TODO exteact on PC too */

	return ZipEntry_ExtractData(e, data, source);
}

static cc_result CCTextures_ExtractZip(struct HttpRequest* req) {
	struct Stream src;
	cc_result res;
	struct ZipEntry entries[64];

	Stream_ReadonlyMemory(&src, req->data, req->size);
	if ((res = Zip_Extract(&src, CCTextures_SelectEntry, CCTextures_ProcessEntry,
							entries, Array_Elems(entries)))) return res;

	return Stream_WriteAllTo(&ccTexPack, req->data, req->size);
}

static void CCTextures_CheckStatus(void) {
	struct HttpRequest item;
	cc_result res;

	if (ccTexturesDownloaded) return;
	if (!Fetcher_Get(ccTexturesReqID, &item)) return;

	ccTexturesDownloaded = true;
	res = CCTextures_ExtractZip(&item);
	if (res) Logger_SysWarn(res, "saving ClassiCube textures");

	HttpRequest_Free(&item);
}

static void CCTextures_ResetState(void) {
	ccTexturesExist      = false;
	ccTexturesDownloaded = false;
}

static const struct AssetSet ccTexsAssetSet = {
	CCTextures_CheckExistence,
	CCTextures_CountMissing,
	CCTextures_DownloadAssets,
	CCTextures_GetRequestName,
	CCTextures_CheckStatus,
	CCTextures_ResetState
};


/*########################################################################################################################*
*----------------------------------------------------default.zip resources------------------------------------------------*
*#########################################################################################################################*/
#define ANIMS_TXT \
"# This file defines the animations used in a texture pack for ClassiCube.\r\n" \
"# Each line is in the format : <TileX> <TileY> <FrameX> <FrameY> <Frame size> <Frames count> <Tick delay>\r\n" \
"# - TileX and TileY are the coordinates of the tile in terrain.png that will be replaced by the animation frames.\r\n" \
"#     Essentially, TileX and TileY are the remainder and quotient of an ID in F10 menu divided by 16\r\n" \
"#     For instance, obsidian texture(37) has TileX of 5, and TileY of 2\r\n" \
"# - FrameX and FrameY are the pixel coordinates of the first animation frame in animations.png.\r\n" \
"# - Frame Size is the size in pixels of an animation frame.\r\n" \
"# - Frames count is the number of used frames.  The first frame is located at\r\n" \
"#     (FrameX, FrameY), the second one at (FrameX + FrameSize, FrameY) and so on.\r\n" \
"# - Tick delay is the number of ticks a frame doesn't change. For instance, delay of 0\r\n" \
"#     means that the tile would be replaced every tick, while delay of 2 means\r\n" \
"#     'replace with frame 1, don't change frame, don't change frame, replace with frame 2'.\r\n" \
"# NOTE: If a file called 'uselavaanim' is in the texture pack, the game instead generates the lava texture animation.\r\n" \
"# NOTE: If a file called 'usewateranim' is in the texture pack, the game instead generates the water texture animation.\r\n" \
"\r\n" \
"# fire\r\n" \
"6 2 0 0 16 32 0"

/* The entries that are required to exist within default.zip */
static struct ResourceZipEntry defaultZipEntries[] = {
	/* classic jar files */
	{ "terrain.png",  RESOURCE_TYPE_PNG  }, { "particles.png",   RESOURCE_TYPE_DATA },
	{ "clouds.png",   RESOURCE_TYPE_DATA }, { "rain.png",        RESOURCE_TYPE_DATA },
	{ "char.png",     RESOURCE_TYPE_DATA }, { "default.png",     RESOURCE_TYPE_DATA }, 
	{ "icons.png",    RESOURCE_TYPE_DATA }, { "gui_classic.png", RESOURCE_TYPE_DATA },
	{ "creeper.png",  RESOURCE_TYPE_DATA }, { "pig.png",         RESOURCE_TYPE_DATA }, 
	{ "sheep.png",    RESOURCE_TYPE_DATA }, { "sheep_fur.png",   RESOURCE_TYPE_DATA },
	{ "skeleton.png", RESOURCE_TYPE_DATA }, { "spider.png",      RESOURCE_TYPE_DATA }, 
	{ "zombie.png",   RESOURCE_TYPE_DATA },
	/* other files */
	{ "snow.png", RESOURCE_TYPE_DATA }, { "chicken.png",    RESOURCE_TYPE_DATA },
	{ "gui.png",  RESOURCE_TYPE_DATA }, { "animations.png", RESOURCE_TYPE_PNG  }, 
	{ "animations.txt", RESOURCE_TYPE_CONST, sizeof(ANIMS_TXT) - 1, (cc_uint8*)ANIMS_TXT },
#ifdef CC_BUILD_MOBILE
	{ "touch.png", RESOURCE_TYPE_DATA }
#endif
};

CC_NOINLINE static struct ResourceZipEntry* ZipEntries_Find(const cc_string* name) {
	struct ResourceZipEntry* e;
	int i;

	for (i = 0; i < Array_Elems(defaultZipEntries); i++) 
	{
		e = &defaultZipEntries[i];
		if (String_CaselessEqualsConst(name, e->filename)) return e;
	}
	return NULL;
}


static cc_result ClassicPatcher_ExtractFiles(struct HttpRequest* req);
static cc_result ModernPatcher_ExtractFiles(struct HttpRequest* req);
static cc_result TerrainPatcher_Process(struct HttpRequest* req);
static cc_result NewTextures_ExtractGui(struct HttpRequest* req);

static cc_result Classic0023Patcher_OldGoldBlock(struct HttpRequest* req);
static cc_result Classic0023Patcher_OldGoldOre(  struct HttpRequest* req);
static cc_result Classic0023Patcher_OldBlackWool(struct HttpRequest* req);
static cc_result Classic0023Patcher_OldGrayWool( struct HttpRequest* req);

/* URLs which data is downloaded from in order to generate the entries in default.zip */
struct ZipfileSource {
	const char* name;
	const char* url;
	cc_result (*Process)(struct HttpRequest* req);
	short size;
	cc_bool downloaded;
	int reqID;
};

#define DEFAULTZIP_0030_ENTRIES_COUNT 4
static struct ZipfileSource defaultZipSources_0030_0023[] = {
	{ "classic jar", "http://launcher.mojang.com/mc/game/c0.30_01c/client/54622801f5ef1bcc1549a842c5b04cb5d5583005/client.jar", ClassicPatcher_ExtractFiles, 291 },
	{ "1.6.2 jar",   "http://launcher.mojang.com/mc/game/1.6.2/client/b6cb68afde1d9cf4a20cbf27fa90d0828bf440a4/client.jar",     ModernPatcher_ExtractFiles, 4621 },
	{ "terrain.png patch", RESOURCE_SERVER "/terrain-patch2.png", TerrainPatcher_Process, 7 },
	{ "gui.png patch",     RESOURCE_SERVER "/gui.png",            NewTextures_ExtractGui, 21 },
	/* 0.0.23 textures */
	{ "0.0.23 gold",  "https://classic.minecraft.net/assets/textures/gold.png",      Classic0023Patcher_OldGoldBlock, 1 },
	{ "0.0.23 ore",   "https://classic.minecraft.net/assets/textures/rock_gold.png", Classic0023Patcher_OldGoldOre,   1 },
	{ "0.0.23 black", "https://classic.minecraft.net/assets/textures/color13.png",   Classic0023Patcher_OldBlackWool, 1 },
	{ "0.0.23 gray",  "https://classic.minecraft.net/assets/textures/color14.png",   Classic0023Patcher_OldGrayWool,  1 },
};
static struct ZipfileSource* defaultZipSources;
static int numDefaultZipSources, numDefaultZipProcessed;

static void MCCTextures_ResetState(void) {
	int i;
	for (i = 0; i < Array_Elems(defaultZipEntries); i++) 
	{
		if (defaultZipEntries[i].type == RESOURCE_TYPE_CONST) continue;

		/* can reuse value.data for value.bmp case too */
		Mem_Free(defaultZipEntries[i].value.data);
		defaultZipEntries[i].value.data = NULL;
		defaultZipEntries[i].size       = 0;
	}
}


/*########################################################################################################################*
*------------------------------------------------default.zip entry generators---------------------------------------------*
*#########################################################################################################################*/
static cc_bool ClassicPatcher_SelectEntry(const cc_string* path) {
	cc_string name = *path;
	Utils_UNSAFE_GetFilename(&name);
	return ZipEntries_Find(&name) != NULL;
}

static cc_result ClassicPatcher_ProcessEntry(const cc_string* path, struct Stream* data, struct ZipEntry* source) {
	static const cc_string guiClassicPng = String_FromConst("gui_classic.png");
	struct ResourceZipEntry* e;
	cc_string name;

	name = *path;
	Utils_UNSAFE_GetFilename(&name);
	if (String_CaselessEqualsConst(&name, "gui.png")) name = guiClassicPng;

	e = ZipEntries_Find(&name);

	/* terrain.png requires special handling */
	if (String_CaselessEqualsConst(path, "terrain.png")) {
		return Png_Decode(&e->value.bmp, data);
	}
	return ZipEntry_ExtractData(e, data, source);
}

static cc_result ClassicPatcher_ExtractFiles(struct HttpRequest* req) {
	struct Stream src;
	cc_result res;
	struct ZipEntry entries[64];
	Stream_ReadonlyMemory(&src, req->data, req->size);
	
	return Zip_Extract(&src, 
			ClassicPatcher_SelectEntry, ClassicPatcher_ProcessEntry,
			entries, Array_Elems(entries));
}

static void PatchTerrainTile(struct Bitmap* src, int srcX, int srcY, int tileX, int tileY) {
	static const cc_string terrainPng = String_FromConst("terrain.png");
	struct ResourceZipEntry* entry    = ZipEntries_Find(&terrainPng);
	struct Bitmap* dst = &entry->value.bmp;
	/* Can happen sometimes happen when allocating memory for terrain.png fails */
	if (!dst->scan0) return;

	Bitmap_UNSAFE_CopyBlock(srcX, srcY, tileX * 16, tileY * 16, src, dst, 16);
}


/* the x,y of tiles in terrain.png which get patched */
static const struct TilePatch { const char* name; cc_uint8 x1,y1, x2,y2; } modern_tiles[12] = {
	{ "assets/minecraft/textures/blocks/sandstone_bottom.png", 9,3 },
	{ "assets/minecraft/textures/blocks/sandstone_normal.png", 9,2 },
	{ "assets/minecraft/textures/blocks/sandstone_top.png", 9,1, },
	{ "assets/minecraft/textures/blocks/quartz_block_lines_top.png", 10,3, 10,1 },
	{ "assets/minecraft/textures/blocks/quartz_block_lines.png", 10,2 },
	{ "assets/minecraft/textures/blocks/stonebrick.png", 4,3 },
	{ "assets/minecraft/textures/blocks/snow.png", 2,3 },
	{ "assets/minecraft/textures/blocks/wool_colored_blue.png",  3,5 },
	{ "assets/minecraft/textures/blocks/wool_colored_brown.png", 2,5 },
	{ "assets/minecraft/textures/blocks/wool_colored_cyan.png",  4,5 },
	{ "assets/minecraft/textures/blocks/wool_colored_green.png", 1,5 },
	{ "assets/minecraft/textures/blocks/wool_colored_pink.png",  0,5 }
};

CC_NOINLINE static const struct TilePatch* ModernPatcher_GetTile(const cc_string* path) {
	int i;
	for (i = 0; i < Array_Elems(modern_tiles); i++) {
		if (String_CaselessEqualsConst(path, modern_tiles[i].name)) return &modern_tiles[i];
	}
	return NULL;
}

static cc_result ModernPatcher_PatchTile(struct Stream* data, const struct TilePatch* tile) {
	struct Bitmap bmp;
	cc_result res;

	if ((res = Png_Decode(&bmp, data))) return res;
	PatchTerrainTile(&bmp, 0, 0, tile->x1, tile->y1);

	/* only quartz needs copying to two tiles */
	if (tile->y2) PatchTerrainTile(&bmp, 0, 0, tile->x2, tile->y2);

	Mem_Free(bmp.scan0);
	return 0;
}


static cc_bool ModernPatcher_SelectEntry(const cc_string* path) {
	return
		String_CaselessEqualsConst(path, "assets/minecraft/textures/environment/snow.png") ||
		String_CaselessEqualsConst(path, "assets/minecraft/textures/entity/chicken.png")   ||
		String_CaselessEqualsConst(path, "assets/minecraft/textures/blocks/fire_layer_1.png") ||
		ModernPatcher_GetTile(path) != NULL;
}

static cc_result ModernPatcher_MakeAnimations(struct Stream* data) {
	static const cc_string animsPng = String_FromConst("animations.png");
	struct ResourceZipEntry* entry;
	struct Bitmap* anim;
	struct Bitmap bmp;
	cc_result res;
	int i;

	entry = ZipEntries_Find(&animsPng);
	anim  = &entry->value.bmp;
	Bitmap_TryAllocate(anim, 512, 16);

	if (!anim->scan0) return ERR_OUT_OF_MEMORY;
	if ((res = Png_Decode(&bmp, data))) return res;

	for (i = 0; i < 512; i += 16) {
		Bitmap_UNSAFE_CopyBlock(0, i, i, 0, &bmp, anim, 16);
	}

	Mem_Free(bmp.scan0); 
	return 0;
}

static cc_result ModernPatcher_ProcessEntry(const cc_string* path, struct Stream* data, struct ZipEntry* source) {
	struct ResourceZipEntry* e;
	const struct TilePatch* tile;
	cc_string name;

	if (String_CaselessEqualsConst(path, "assets/minecraft/textures/environment/snow.png")
		|| String_CaselessEqualsConst(path, "assets/minecraft/textures/entity/chicken.png")) {
		name = *path;
		Utils_UNSAFE_GetFilename(&name);

		e = ZipEntries_Find(&name);
		return ZipEntry_ExtractData(e, data, source);
	}

	if (String_CaselessEqualsConst(path, "assets/minecraft/textures/blocks/fire_layer_1.png")) {
		return ModernPatcher_MakeAnimations(data);
	}

	tile = ModernPatcher_GetTile(path);
	return ModernPatcher_PatchTile(data, tile);
}

static cc_result ModernPatcher_ExtractFiles(struct HttpRequest* req) {
	struct Stream src;
	struct ZipEntry entries[64];
	Stream_ReadonlyMemory(&src, req->data, req->size);

	return Zip_Extract(&src, 
			ModernPatcher_SelectEntry, ModernPatcher_ProcessEntry,
			entries, Array_Elems(entries));
}


static cc_result TerrainPatcher_Process(struct HttpRequest* req) {
	struct Bitmap bmp;
	struct Stream src;
	cc_result res;

	Stream_ReadonlyMemory(&src, req->data, req->size);
	if ((res = Png_Decode(&bmp, &src))) return res;

	PatchTerrainTile(&bmp,  0,0, 3,3);
	PatchTerrainTile(&bmp, 16,0, 6,3);
	PatchTerrainTile(&bmp, 32,0, 6,2);

	PatchTerrainTile(&bmp,  0,16,  5,3);
	PatchTerrainTile(&bmp, 16,16,  6,5);
	PatchTerrainTile(&bmp, 32,16, 11,0);

	Mem_Free(bmp.scan0);
	return 0;
}

static cc_result NewTextures_ExtractGui(struct HttpRequest* req) {
	static const cc_string guiPng = String_FromConst("gui.png");
	struct ResourceZipEntry* entry = ZipEntries_Find(&guiPng);

	entry->value.data = req->data;
	entry->size       = req->size;

	req->data = NULL; /* don't free memory yet */
	return 0;
}

static cc_result Classic0023Patcher_PatchBlocks(struct HttpRequest* req, const int* targets) {
	struct Bitmap bmp;
	struct Stream src;
	cc_result res;

	Stream_ReadonlyMemory(&src, req->data, req->size);
	if ((res = Png_Decode(&bmp, &src))) return res;

	while (*targets)
	{
		PatchTerrainTile(&bmp, 0,0, *targets >> 8, *targets & 0xFF);
		targets++;
	}

	Mem_Free(bmp.scan0);
	return 0;
}

static cc_result Classic0023Patcher_OldGoldBlock(struct HttpRequest* req) {
	static const int targets[] = { (8 << 8) | 1, (8 << 8) | 2, (8 << 8) | 3, 0 };

	return Classic0023Patcher_PatchBlocks(req, targets);
}

static cc_result Classic0023Patcher_OldGoldOre(struct HttpRequest* req) {
	static const int targets[] = { (0 << 8) | 2, 0 };

	return Classic0023Patcher_PatchBlocks(req, targets);
}

static cc_result Classic0023Patcher_OldBlackWool(struct HttpRequest* req) {
	static const int targets[] = { (13 << 8) | 4, 0 };

	return Classic0023Patcher_PatchBlocks(req, targets);
}

static cc_result Classic0023Patcher_OldGrayWool(struct HttpRequest* req) {
	static const int targets[] = { (14 << 8) | 4, 0 };

	return Classic0023Patcher_PatchBlocks(req, targets);
}


/*########################################################################################################################*
*-----------------------------------------------Minecraft Classic texture assets------------------------------------------*
*#########################################################################################################################*/
static cc_bool allZipEntriesExist;
static int zipEntriesFound;

static cc_bool DefaultZip_SelectEntry(const cc_string* path) {
	cc_string name = *path;
	Utils_UNSAFE_GetFilename(&name);

	if (ZipEntries_Find(&name)) zipEntriesFound++;
	return false;
}

static void MCCTextures_CheckExistence(void) {
	cc_string path  = String_FromReadonly(Game_Version.DefaultTexpack);
	zipEntriesFound = 0;

	ZipFile_InspectEntries(&path, DefaultZip_SelectEntry);
	/* >= in case somehow have say "gui.png", "GUI.png" */
	allZipEntriesExist = zipEntriesFound >= Array_Elems(defaultZipEntries);

	/* Need touch.png from ClassiCube textures */
	if (!allZipEntriesExist) ccTexturesExist = false;
}

static void MCCTextures_CountMissing(void) {
	int i;
	if (allZipEntriesExist) return;

	/* old gold texture only needed in 0.0.23 and earlier */
	if (Game_Version.Version > VERSION_0023) {
		numDefaultZipSources = DEFAULTZIP_0030_ENTRIES_COUNT;
		defaultZipSources    = defaultZipSources_0030_0023;
	} else {
		numDefaultZipSources = Array_Elems(defaultZipSources_0030_0023);
		defaultZipSources    = defaultZipSources_0030_0023;
	}

	for (i = 0; i < numDefaultZipSources; i++) {
		Resources_MissingCount++;
		Resources_MissingSize += defaultZipSources[i].size;
	}
}


/*########################################################################################################################*
*------------------------------------------Minecraft Classic texture assets fetching -------------------------------------*
*#########################################################################################################################*/
static void MCCTextures_DownloadAssets(void) {
	cc_string url;
	int i;
	if (allZipEntriesExist) return;
	numDefaultZipProcessed = 0;

	for (i = 0; i < numDefaultZipSources; i++)
	{
		url = String_FromReadonly(defaultZipSources[i].url);
		defaultZipSources[i].reqID = Http_AsyncGetData(&url, 0);
		defaultZipSources[i].downloaded = false;
	}
}

static const char* MCCTextures_GetRequestName(int reqID) {
	int i;
	for (i = 0; i < numDefaultZipSources; i++) 
	{
		if (reqID == defaultZipSources[i].reqID) return defaultZipSources[i].name;
	}
	return NULL;
}


/*########################################################################################################################*
*------------------------------------------Minecraft Classic texture assets processing -----------------------------------*
*#########################################################################################################################*/
static void MCCTextures_CreateDefaultZip(void) {
	cc_string path = String_FromReadonly(Game_Version.DefaultTexpack);
	ZipFile_Create(&path, defaultZipEntries, Array_Elems(defaultZipEntries));
	MCCTextures_ResetState();
}

static void MCCTextures_CheckSource(struct ZipfileSource* source) {
	struct HttpRequest item;
	cc_result res;
	if (!Fetcher_Get(source->reqID, &item)) return;
	
	source->downloaded = true;
	res = source->Process(&item);

	if (res) {
		cc_string name = String_FromReadonly(source->name);
		Logger_SysWarn2(res, "making", &name);
	}
	HttpRequest_Free(&item);

	if (++numDefaultZipProcessed < numDefaultZipSources) return;
	MCCTextures_CreateDefaultZip();
}

static void MCCTextures_CheckStatus(void) {
	int i;
	for (i = 0; i < numDefaultZipSources; i++) 
	{
		if (defaultZipSources[i].downloaded) continue;
		MCCTextures_CheckSource(&defaultZipSources[i]);
	}
}

static const struct AssetSet mccTexsAssetSet = {
	MCCTextures_CheckExistence,
	MCCTextures_CountMissing,
	MCCTextures_DownloadAssets,
	MCCTextures_GetRequestName,
	MCCTextures_CheckStatus,
	MCCTextures_ResetState
};


/*########################################################################################################################*
*-----------------------------------------------------------Fetcher-------------------------------------------------------*
*#########################################################################################################################*/
cc_bool Fetcher_Working, Fetcher_Completed, Fetcher_Failed;
int Fetcher_Downloaded;
FetcherErrorCallback Fetcher_ErrorCallback;

static const struct AssetSet* const asset_sets[] = {
	&ccTexsAssetSet,
	&mccTexsAssetSet,
#ifndef CC_BUILD_NOMUSIC
	&mccMusicAssetSet,
#endif
#ifndef CC_BUILD_NOSOUNDS
	&mccSoundAssetSet
#endif
};

static void ResetState() {
	int i;
	Resources_MissingCount = 0;
	Resources_MissingSize  = 0;

	for (i = 0; i < Array_Elems(asset_sets); i++)
	{
		asset_sets[i]->ResetState();
	}
}

void Resources_CheckExistence(void) {
	int i;
	ResetState();

	for (i = 0; i < Array_Elems(asset_sets); i++)
	{
		asset_sets[i]->CheckExistence();
	}

	for (i = 0; i < Array_Elems(asset_sets); i++)
	{
		asset_sets[i]->CountMissing();
	}
}

const char* Fetcher_RequestName(int reqID) {
	const char* name;
	int i;

	for (i = 0; i < Array_Elems(asset_sets); i++)
	{
		if ((name = asset_sets[i]->GetRequestName(reqID))) return name;
	}
	return NULL;
}

void Fetcher_Run(void) {
	int i;

	Fetcher_Failed     = false;
	Fetcher_Downloaded = 0;
	Fetcher_Working    = true;
	Fetcher_Completed  = false;

	for (i = 0; i < Array_Elems(asset_sets); i++)
	{
		asset_sets[i]->DownloadAssets();
	}
}

static void Fetcher_Finish(void) {
	Fetcher_Completed = true;
	Fetcher_Working   = false;
	ResetState();
}

static void Fetcher_Fail(struct HttpRequest* item) {
	Http_ClearPending();

	/* Only show error for first failed download */
	if (!Fetcher_Failed) Fetcher_ErrorCallback(item);
	Fetcher_Failed = true;
}

CC_NOINLINE static cc_bool Fetcher_Get(int reqID, struct HttpRequest* item) {
	if (!Http_GetResult(reqID, item)) return false;

	if (item->success) {
		Fetcher_Downloaded++;
		return true;
	}

	Fetcher_Fail(item);
	HttpRequest_Free(item);
	
	Fetcher_Finish();
	return false;
}

/* TODO: Implement this.. */
/* TODO: How expensive is it to constantly do 'Get' over and over */
void Fetcher_Update(void) {
	int i;
	for (i = 0; i < Array_Elems(asset_sets); i++)
	{
		asset_sets[i]->CheckStatus();
	}

	if (Fetcher_Downloaded != Resources_MissingCount) return; 
	Fetcher_Finish();
}
#endif
