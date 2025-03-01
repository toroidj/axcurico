/*-----------------------------------------------------------------------------
	 Cursor / Icon Susie Plug-in			Copyright (c) TORO
 ----------------------------------------------------------------------------*/
#include <windows.h>
#include "torowin.h"

#define DEBUGINFO 0

// Susie Plug-in 関連の定義 ---------------------------------------------------
// エラー定義
#define SUSIEERROR_NOERROR       0
#define SUSIEERROR_NOTSUPPORT   -1
#define SUSIEERROR_USERCANCEL    1
#define SUSIEERROR_UNKNOWNFORMAT 2
#define SUSIEERROR_BROKENDATA    3
#define SUSIEERROR_EMPTYMEMORY   4
#define SUSIEERROR_FAULTMEMORY   5
#define SUSIEERROR_FAULTREAD     6
#define SUSIEERROR_RESERVED      7 // Susie 内部では Window 関係
#define SUSIEERROR_INTERNAL      8
#define SUSIEERROR_FILEWRITE     9 // Susie 内部で使用
#define SUSIEERROR_EOF           10 // Susie 内部で使用

// flag 定義
#define SUSIE_SOURCE_MASK 7
#define SUSIE_SOURCE_DISK 0
#define SUSIE_SOURCE_MEM 1
#define SUSIE_SOURCE_IGNORECASE 0x80
#define SUSIE_DEST_MASK 0x700
#define SUSIE_DEST_DISK 0
#define SUSIE_DEST_MEM 0x100
#define SUSIE_DEST_REJECT_UNKNOWN_TYPE 0x800
#define SUSIE_DEST_EXTRA_OPTION 0x1000

// その他定義
#define SUSIE_CHECK_SIZE (2 * 1024)
#define SUSIE_PATH_MAX 200

// コールバック
typedef int (__stdcall *SUSIE_PROGRESS)(int nNum, int nDenom, LONG_PTR lData);

// Susie 用の UNIX 時刻
typedef ULONG_PTR susie_time_t;

// 書庫関係構造体
#pragma pack(push, 1)
typedef struct {
	unsigned char  method[8];  // 圧縮法の種類
	ULONG_PTR      position;   // ファイル上での位置
	ULONG_PTR      compsize;   // 圧縮されたサイズ
	ULONG_PTR      filesize;   // 元のファイルサイズ
	susie_time_t   timestamp;  // ファイルの更新日時
	char           path[SUSIE_PATH_MAX]; // 相対パス
	char           filename[SUSIE_PATH_MAX]; // ファイル名
	unsigned long  crc; // CRC
	#ifdef _WIN64
	   // 64bit版の構造体サイズは444bytesですが、実際のサイズは
	   // アラインメントにより448bytesになります。環境によりdummyが必要です。
	   char dummy[4];
	#endif
} SUSIE_FINFO;

typedef struct {
	unsigned char  method[8]; // 圧縮法の種類
	ULONG_PTR      position;  // ファイル上での位置
	ULONG_PTR      compsize;  // 圧縮されたサイズ
	ULONG_PTR      filesize;  // 元のファイルサイズ
	susie_time_t   timestamp; // ファイルの更新日時
	WCHAR          path[SUSIE_PATH_MAX]; // 相対パス
	WCHAR          filename[SUSIE_PATH_MAX]; // ファイル名
	unsigned long  crc; // CRC
#ifdef _WIN64
	   // 64bit版の構造体サイズは844bytesですが、実際のサイズは
	   // アラインメントにより848bytesになります。環境によりdummyが必要です。
	char dummy[4]; // アラインメント
#endif
} SUSIE_FINFOW;
#pragma pack(pop)

// リソース関連の定義 ---------------------------------------------------------
#pragma pack(push, 1)
// ICO/CUR ファイルのヘッダ
typedef struct {
	BYTE bWidth;
	BYTE bHeight;
	BYTE bColorCount;
	BYTE bReserved;
	WORD wPlanes;
	WORD wBitCount;
	DWORD dwBytesInRes;
	DWORD dwImageOffset;
} ICONDIRENTRY;

typedef struct {
	WORD idReserved;
	WORD idType;
	WORD idCount;
	ICONDIRENTRY idEntries[1];
} ICONDIR;
#pragma pack(pop)

const char RiffHeader[4] = {'R', 'I', 'F', 'F'};
const char ACONHeader[4] = {'A', 'C', 'O', 'N'};

// ----------------------------------------------------------------------------
typedef struct {
	SUSIE_FINFOW *first, *infos, *max;
	DWORD allocsize;
	BOOL UseCache; // cache mode の為、メモリ解放を禁止するかどうか
} SUSIEINFOLIST;

void InitSusieInfoList(SUSIEINFOLIST &sil)
{
	sil.allocsize = 4 * sizeof(SUSIE_FINFOW);
	sil.first = (SUSIE_FINFOW *)HeapAlloc(GetProcessHeap(), 0, sil.allocsize);
	if ( sil.first == NULL ){ // 確保失敗
		sil.infos = sil.max = NULL;
		sil.allocsize = 0;
	}else{
		sil.infos = sil.first;
		sil.max = (SUSIE_FINFOW *)(BYTE *)((BYTE *)sil.first + sil.allocsize);
	}
	sil.UseCache = FALSE;
}

SUSIE_FINFOW *CheckSizeSusieInfoList(SUSIEINFOLIST &sil)
{
	SUSIE_FINFOW *newinfos;

	if ( sil.infos < sil.max ) return sil.infos++;
	newinfos = (SUSIE_FINFOW *)HeapReAlloc(GetProcessHeap(), 0, sil.first, sil.allocsize * 2);
	if ( newinfos == NULL ) return NULL;
	sil.allocsize *= 2;
	sil.infos = newinfos + (sil.infos - sil.first);
	sil.first = newinfos;
	sil.max = (SUSIE_FINFOW *)(BYTE *)((BYTE *)newinfos + sil.allocsize);
	return sil.infos++;
}

void EndListSusieInfoList(SUSIEINFOLIST &sil)
{
	DWORD size;
	SUSIE_FINFOW *newinfos;

	if ( sil.first == NULL ) return;
	size = ((BYTE *)sil.infos - (BYTE *)sil.first) + sizeof(SUSIE_FINFOW);
	newinfos = (SUSIE_FINFOW *)HeapReAlloc(GetProcessHeap(), 0, sil.first, size);
	if ( newinfos == NULL ){
		if ( sil.infos != sil.first ) sil.infos--;
	}else{
		sil.allocsize = size;
		sil.first = newinfos;
		sil.max = (SUSIE_FINFOW *)(BYTE *)((BYTE *)sil.first + size);
		sil.infos = sil.max - 1;
	}
	sil.infos->method[0] = '\0';
	sil.infos++;
}

void FreeSusieInfoList(SUSIEINFOLIST &sil)
{
	if ( sil.first == NULL ) return;
	if ( sil.UseCache != FALSE ) return; // cache mode なので解放しない
	HeapFree(GetProcessHeap(), 0, sil.first);
}

HLOCAL CopySusieInfoList(SUSIEINFOLIST &sil)
{
	HLOCAL hImage;

	if ( sil.first == NULL ) return NULL;
	hImage = LocalAlloc(0, sil.allocsize);
	if ( hImage == NULL ) return NULL;
	memcpy(LocalLock(hImage), sil.first, sil.allocsize);
	LocalUnlock(hImage);
	return hImage;
}

DWORD USEFASTCALL GetBigEndDWORD(void *ptr)
{
	DWORD value;

	value = *(DWORD *)ptr;
	return	(value << 24) |
			((value & 0xff00) << 8) |
			((value >> 8) & 0xff00) |
			(value >> 24);
}

// ----------------------------------------------------------------------------
//HINSTANCE hDLLInst;

#define MAX_RT_DEF 25

struct {
	SUSIEINFOLIST sil;
	WCHAR filename[MAX_PATH];
	DWORD ThreadID;
} CachedInfo = {{NULL, NULL, NULL, 0}, L"", 0};
/*-----------------------------------------------------------------------------
	dll の初期化／終了処理
-----------------------------------------------------------------------------*/
BOOL WINAPI DLLEntry(HINSTANCE, DWORD reason, LPVOID)
{
	switch (reason){
		// DLL 初期化 ---------------------------------------------------------
//		case DLL_PROCESS_ATTACH:
//			hDLLInst = hInst;
//			break;

		// DLL の終了処理 -----------------------------------------------------
		case DLL_PROCESS_DETACH:
			CachedInfo.sil.UseCache = FALSE;
			FreeSusieInfoList(CachedInfo.sil);
			break;
	}
	return TRUE;
}
/*-----------------------------------------------------------------------------
	Plug-in interface
-----------------------------------------------------------------------------*/
#define InfoCount 8
const char *InfoText[InfoCount] = {
	"00AM",
	"Cursor / Icon Susie Plug-in 1.3 (c)2017-2025 TORO",
	"*.ANI",
	"Animated cursor",
	"*.CUR",
	"Cursor",
	"*.ICO",
	"Icon"
};

extern "C" int __stdcall GetPluginInfo(int infono, LPSTR buf, int buflen)
{
	const char *src, *srcmax;
	char *dest;

	dest = buf;
	if ( buflen > 0 ){
		if ( (DWORD)infono < (DWORD)InfoCount ){
			src = InfoText[infono];
			srcmax = src + buflen - 1;
			while( src < srcmax ){
				char code;

				code = *src++;
				if ( code == '\0' ) break;
				*dest++ = code;
			}
		}
		*dest = '\0';
	}
	return dest - buf;
}

extern "C" int __stdcall GetPluginInfoW(int infono, LPWSTR buf, int buflen)
{
	char bufA[80];
	int len;

	len = GetPluginInfo(infono, bufA, sizeof(bufA));
	if ( len <= 0 ){
		if ( buflen > 0 ) buf[0] = '\0';
		return 0;
	}
	len = AnsiToUnicode(bufA, buf, buflen);
	buf[buflen - 1] = '\0';
	return len - 1;
}
//-----------------------------------------------------------------------------
extern "C" int __stdcall IsSupportedW(LPCWSTR filename, const void *dw)
{
	BYTE buf[SUSIE_CHECK_SIZE];
	const BYTE *header;

	if ( (CachedInfo.sil.first != NULL) &&
		 (filename != NULL) &&
		 wcscmp(CachedInfo.filename, filename) ){
		FreeSusieInfoList(CachedInfo.sil);
		CachedInfo.sil.first = NULL;
	}

	if ( (DWORD_PTR)dw & ~(DWORD_PTR)0xffff ){ // 2K メモリイメージ
		header = (const BYTE *)dw;
	}else{ // ファイルハンドル
		DWORD size;

		buf[0] = '\1'; // ファイルサイズが 0 の時、誤判定しないようなデータ
		if ( ReadFile((HANDLE)dw, buf, sizeof(buf), &size, NULL) == FALSE ){
			return 0; // 読み込み失敗
		}
		header = (const BYTE *)buf;
	}
	// ファイル内容による判別
	// .ani
	if ( !memcmp(header, RiffHeader, sizeof(RiffHeader)) &&
		 !memcmp(header + 8, ACONHeader, sizeof(ACONHeader)) ){
		return 1;
	}
	// .cur/.ico
	if ( (!memcmp(header, "\0\0\1", 4) || !memcmp(header, "\0\0\2", 4)) &&
		 (*(WORD *)(header + 4) > 1) && (*(WORD *)(header + 4) < 20) ){
		return 1;
	}
	return 0;
}

extern "C" int __stdcall IsSupported(LPCSTR, const void *dw)
{
	return IsSupportedW(NULL, dw);
}

//-----------------------------------------------------------------------------
extern "C" int __stdcall GetArchiveInfoLocal(const char *buf, LONG_PTR len, unsigned int flag, SUSIEINFOLIST &sil, BOOL unicode)
{
	HANDLE hFile;
	DWORD sizeL, sizeH;		// ファイルの大きさ
	char *image, *imgptr, *imgmax;
	DWORD ThreadID;
	DWORD result;
	WCHAR filenameW[MAX_PATH];
	DWORD *seqptr = NULL;
	DWORD_PTR frame_rate = 0, frame_time = 0;
	DWORD iconid = 1;

	ThreadID = GetCurrentThreadId();
	switch ( flag & SUSIE_SOURCE_MASK ){
		case SUSIE_SOURCE_DISK:
			// キャッシュ用ファイル名を用意
			if ( unicode ){
				wcsncpy(filenameW, (const WCHAR *)buf, MAX_PATH);
			}else{
				AnsiToUnicode(buf, filenameW, MAX_PATH);
			}
			filenameW[MAX_PATH - 1] = '\0';

			// キャッシュがあるなら利用する
			if ( (CachedInfo.sil.first != NULL) &&
				 (CachedInfo.ThreadID == ThreadID) &&
				 !wcscmp(CachedInfo.filename, filenameW) ){
				sil = CachedInfo.sil;
				// sil.UseCache = TRUE; すでに TRUE
				return SUSIEERROR_NOERROR;
			}

			// ファイルから読み込む
			if ( unicode ){
				hFile = CreateFileW((const WCHAR *)buf, GENERIC_READ,
						FILE_SHARE_WRITE | FILE_SHARE_READ,
						NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
			}else{
				hFile = CreateFileA(buf, GENERIC_READ,
						FILE_SHARE_WRITE | FILE_SHARE_READ,
						NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
			}
			if ( hFile == INVALID_HANDLE_VALUE ) return SUSIEERROR_FAULTREAD;
			sizeL = GetFileSize(hFile, &sizeH);
			if ( sizeH || (sizeL >= 0xffff0000) ) return SUSIEERROR_EMPTYMEMORY;

			image = (char *)HeapAlloc(GetProcessHeap(), 0, sizeL + 0x100);
			if ( image == NULL ){
				CloseHandle(hFile);
				return SUSIEERROR_EMPTYMEMORY;
			}
			if ( len != 0 ){
				LONG high;
				#ifdef _WIN64
					high = len >> 32;
				#else
					high = 0;
				#endif
				SetFilePointer(hFile, (DWORD)len, &high, FILE_BEGIN);
			}
			result = ReadFile(hFile, image, sizeL, &sizeL, NULL) ?
					NO_ERROR : GetLastError();
			CloseHandle(hFile);
			if ( (result != NO_ERROR) ||
				 !(!memcmp(image, "\0\0\1", 4) ||
				   !memcmp(image, "\0\0\2", 4) ||
				   (!memcmp(image, RiffHeader, sizeof(RiffHeader)) &&
					!memcmp(image + 8, ACONHeader, sizeof(ACONHeader)))) ){
				HeapFree(GetProcessHeap(), 0, image);
				return SUSIEERROR_FAULTREAD;
			}
			break;

		case SUSIE_SOURCE_MEM:
			image = (char *)buf;
			if ( !(!memcmp(image, "\0\0\1", 4) ||
					   !memcmp(image, "\0\0\2", 4) ||
					   (!memcmp(image, RiffHeader, sizeof(RiffHeader)) &&
						!memcmp(image + 8, ACONHeader, sizeof(ACONHeader)))) ){
				return SUSIEERROR_FAULTREAD;
			}
			sizeL = len;
			hFile = NULL;
			break;

		default:
			return SUSIEERROR_NOTSUPPORT;
	}

	InitSusieInfoList(sil);
	if ( (memcmp(image, "\0\0\1", 4) == 0) || (memcmp(image, "\0\0\2", 4) == 0) ){
		ICONDIRENTRY *idir;
		SUSIE_FINFOW *infos;
		DWORD icons, left;
		const WCHAR *nameformat;

		idir = ((ICONDIR *)image)->idEntries;
		left = ((ICONDIR *)image)->idCount;
		icons = left + 1;
		nameformat = image[2] == 1 ? L"%03d(%dx%d-%d).ico" : L"%03d(%dx%d-%d).cur";
		for ( ; left != 0 ; left--, idir++ ){
			int ColorCount;
			DWORD offset;

			if ( NULL == (infos = CheckSizeSusieInfoList(sil)) ) break;

			offset = idir->dwImageOffset;
			if ( sizeL > (DWORD_PTR)(offset + 0x40) ){
				ColorCount = *(WORD *)(BYTE *)((BYTE *)image + offset + 14);
			}else{
				ColorCount = idir->bColorCount ;
			}
			wsprintfW(infos->filename, nameformat,
					icons - left, idir->bWidth, idir->bHeight, ColorCount);
			infos->crc = infos->position = idir->dwImageOffset;
			infos->compsize = idir->dwBytesInRes;
			infos->filesize = idir->dwBytesInRes + sizeof(ICONDIR);
			infos->timestamp = 0;
			infos->path[0] = '\0';
			strcpy((char *)infos->method, image[2] == 1 ? "icon" : "cursor");
			if ( (idir->bWidth == 0) &&
				 (sizeL > (DWORD_PTR)(offset + 0x40)) &&
				 (memcmp((BYTE *)image + offset + 12, "IHDR", 4) == 0) ){
				wsprintfW(infos->filename, nameformat,
						icons - left,
						GetBigEndDWORD( (BYTE *)image + offset + 16 ),
						GetBigEndDWORD( (BYTE *)image + offset + 20 ),
						*(BYTE *)((BYTE *)image + offset + 24));
			}
		}
		goto last;
	}

	imgptr = image + 12;
	imgmax = image + sizeL - 8;
	while ( imgptr < imgmax ){
		DWORD chunksize;

		chunksize = *(DWORD *)(imgptr + 4);
		if ( !memcmp(imgptr, "seq ", 4) && (seqptr == NULL) ){
			seqptr = (DWORD *)(BYTE *)(imgptr + 4);
		}else
		if ( !memcmp(imgptr, "LIST", 4) && !memcmp(imgptr + 8, "fram", 4) ){
			char *frameptr, *framemax;

			frameptr = imgptr + 12;
			framemax = imgptr + chunksize + 8;
			if ( framemax > imgmax ) framemax = imgmax;
			while ( frameptr < framemax ){
				DWORD datasize;

				datasize = *(DWORD *)(frameptr + 4);
				if ( ((frameptr + 8 + datasize) <= (framemax + 8) ) &&
					 !memcmp(frameptr, "icon", 4) ){
					SUSIE_FINFOW *infos;

					if ( NULL == (infos = CheckSizeSusieInfoList(sil)) ) break;
					wsprintfW(infos->filename, L"raw%03d.cur", iconid++);
					wcscpy(infos->path, L"raw\\");
					infos->position = iconid;
					infos->compsize = infos->filesize = datasize;
					infos->timestamp = frame_time;
					strcpy((char *)infos->method, "anicur");
					infos->crc = (frameptr + 8) - image;
					iconid++;
					frame_time += frame_rate;
				}
				frameptr += datasize + 8;
			}
		}else if ( !memcmp(imgptr, "anih", 4) ){
			DWORD frame_count = *(DWORD *)(BYTE *)(imgptr + 0x10); // dwSteps
			if ( frame_count > 1 ){
				frame_rate = (DWORD_PTR)*(DWORD *)(BYTE *)(imgptr + 0x24); // dwDefaultRate(1/60s)
				if ( frame_rate == 0 ) frame_rate = 2;
			}
		}
		imgptr += chunksize + 8;
	}
	if ( seqptr != NULL ){
		DWORD *seqmax = (DWORD *)(BYTE *)((BYTE *)seqptr + 4 + *seqptr - 4 );
		DWORD seqid = 1;

		if ( (char *)seqmax > (imgmax + 8) ){
			seqmax = (DWORD *)(BYTE *)(imgmax + 8);
		}
		seqptr++;
		while ( seqptr <= seqmax ){
			SUSIE_FINFOW *infos;
			DWORD id;

			if ( NULL == (infos = CheckSizeSusieInfoList(sil)) ) break;
			wsprintfW(infos->filename, L"%03d.cur", seqid);
			id = *seqptr++;
			if ( id < iconid ){
				infos->position = iconid + seqid;
				infos->compsize = infos->filesize = sil.first[id].filesize;
			}else{
				infos->position = 0;
				infos->compsize = infos->filesize = 0;
			}
			infos->timestamp = 0;
			infos->path[0] = '\0'; // pathありのときは"dir\dir\"形式
			strcpy((char *)infos->method, "anicur");
			infos->crc = sil.first[id].crc;
			seqid++;
		}
	}
last:
	EndListSusieInfoList(sil);
	if ( hFile != NULL ) HeapFree(GetProcessHeap(), 0, image);

	if ( (flag & SUSIE_SOURCE_MASK) == SUSIE_SOURCE_DISK ){ // filename のときは、キャッシュする
		CachedInfo.ThreadID = ThreadID;
		wcscpy(CachedInfo.filename, filenameW);
		sil.UseCache = TRUE;
		CachedInfo.sil = sil;
	}
	return SUSIEERROR_NOERROR;
}

extern "C" int __stdcall GetArchiveInfoW(LPCWSTR buf, LONG_PTR len, unsigned int flag, HLOCAL *lphInf)
{
	int result;
	SUSIEINFOLIST sil;

	result = GetArchiveInfoLocal((const char *)buf, len, flag, sil, TRUE);
	if ( result != SUSIEERROR_NOERROR ){
		*lphInf = NULL;
		return result;
	}
	*lphInf = CopySusieInfoList(sil);
	FreeSusieInfoList(sil);
	return SUSIEERROR_NOERROR;
}

extern "C" int __stdcall GetArchiveInfo(LPCSTR buf, LONG_PTR len, unsigned int flag, HLOCAL *lphInf)
{
	int result;
	SUSIEINFOLIST sil;
	HLOCAL hImage;

	result = GetArchiveInfoLocal(buf, len, flag, sil, FALSE);
	if ( result != SUSIEERROR_NOERROR ){
		*lphInf = NULL;
		return result;
	}

	if ( sil.first == NULL ){
		hImage = NULL;
	} else{ // SUSIE_FINFOW → SUSIE_FINFO 変換
		int infocount;
		int i;
		SUSIE_FINFO *infosA;
		SUSIE_FINFOW *infosW;

		infocount = sil.allocsize / sizeof(SUSIE_FINFOW);
		hImage = LocalAlloc(0, infocount * sizeof(SUSIE_FINFO));
		if ( hImage == NULL ){
			*lphInf = NULL;
			return SUSIEERROR_EMPTYMEMORY;
		}

		infosA = (SUSIE_FINFO *)LocalLock(hImage);
		infosW = sil.first;

		for ( i = 0; i < infocount; i++ ){
			memcpy(infosA, infosW, (8 * sizeof(unsigned char)) + (sizeof(ULONG_PTR) * 3) + sizeof(susie_time_t));
			UnicodeToAnsi(infosW->path, infosA->path, SUSIE_PATH_MAX);
			infosA->path[SUSIE_PATH_MAX - 1] = '\0';
			UnicodeToAnsi(infosW->filename, infosA->filename, SUSIE_PATH_MAX);
			infosA->filename[SUSIE_PATH_MAX - 1] = '\0';
			infosA->crc = infosW->crc;
			infosA++;
			infosW++;
		}
		LocalUnlock(hImage);
	}
	*lphInf = hImage;
	return SUSIEERROR_NOERROR;
}

int __stdcall GetFileLocal(LPCSTR src, LONG_PTR len, LPSTR dest, unsigned int flag, SUSIE_PROGRESS progressCallback, LONG_PTR lData, BOOL unicode)
{
	SUSIEINFOLIST sil;
	SUSIE_FINFOW *infos;
	char *bin;
	HANDLE hFile;
	DWORD header = 0;
	HLOCAL himage = NULL;
	int result;

	if ( ((flag & SUSIE_DEST_MASK) > SUSIE_DEST_MEM) ||
		 ((flag & SUSIE_SOURCE_MASK) > SUSIE_SOURCE_MEM) ){
		return SUSIEERROR_NOTSUPPORT;
	}
	result = GetArchiveInfoLocal(src, len, flag, sil, unicode);
	if ( result != SUSIEERROR_NOERROR ) return result;

	if ( progressCallback != NULL ) progressCallback(0, 100, lData);
	infos = sil.first;

	while( infos->method[0] != '\0' ){
		if ( infos->position != len ){
			infos++;
			continue;
		}
		if ( infos->method[0] != 'a' ){ // icon/cursor
			header = sizeof(ICONDIR);
		}

		himage = LocalAlloc(0, infos->filesize);
		if ( himage == NULL ) break;
		bin = (char *)LocalLock(himage);

		if ( (flag & SUSIE_SOURCE_MASK) == SUSIE_SOURCE_DISK ){
			DWORD temp;

			if ( unicode ){
				hFile = CreateFileW((const WCHAR *)src, GENERIC_READ,
						FILE_SHARE_WRITE | FILE_SHARE_READ,
						NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
			}else{
				hFile = CreateFileA(src, GENERIC_READ,
						FILE_SHARE_WRITE | FILE_SHARE_READ,
						NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
			}
			if ( hFile == INVALID_HANDLE_VALUE ) break;
			temp = 0;
			if ( SetFilePointer(hFile, infos->crc, (PLONG)&temp, FILE_BEGIN) != 0xffffffff ){
				ReadFile(hFile, bin + header, infos->compsize, &temp, NULL);
				if ( temp != infos->compsize ) temp = 0;
			}
			CloseHandle(hFile);
			if ( temp == 0 ) break;
		}else{ // SUSIE_SOURCE_MEM
			if ( infos->compsize > (DWORD)len ) break;
			memcpy(bin + header, (const char *)src + infos->crc, infos->compsize);
		}

		if ( header == sizeof(ICONDIR) ){ // RT_ICON
			((ICONDIR *)bin)->idReserved = 0;
			((ICONDIR *)bin)->idType = 1;
			((ICONDIR *)bin)->idCount = 1;

			if ( *((BYTE *)bin + sizeof(ICONDIR)) == 0x89 ){ // png
				((ICONDIR *)bin)->idEntries[0].bWidth =
					((ICONDIR *)bin)->idEntries[0].bHeight =
					((ICONDIR *)bin)->idEntries[0].bColorCount = 0;
				((ICONDIR *)bin)->idEntries[0].wBitCount = 0;
			}else{ // bmp
				BITMAPINFOHEADER *bih;

				bih = (BITMAPINFOHEADER *)(bin + sizeof(ICONDIR));
				((ICONDIR *)bin)->idEntries[0].bWidth = (BYTE)bih->biWidth;
				((ICONDIR *)bin)->idEntries[0].bHeight = (BYTE)(bih->biHeight / 2);
				((ICONDIR *)bin)->idEntries[0].bColorCount = (BYTE)(1 << bih->biBitCount);
				((ICONDIR *)bin)->idEntries[0].wBitCount = bih->biBitCount;
			}
			((ICONDIR *)bin)->idEntries[0].bReserved = 0;
			((ICONDIR *)bin)->idEntries[0].wPlanes = 0;
			((ICONDIR *)bin)->idEntries[0].dwBytesInRes = infos->compsize;
			((ICONDIR *)bin)->idEntries[0].dwImageOffset = sizeof(ICONDIR);
		}

		result = SUSIEERROR_NOERROR;
		if ( flag & SUSIE_DEST_MASK ){ // SUSIE_DEST_MEM
			LocalUnlock(himage);
			*(HLOCAL *)dest = himage;
		}else{ // SUSIE_DEST_DISK
			HANDLE hDestFile;

			if ( unicode ){
				WCHAR destpathW[MAX_PATH];

				wsprintfW(destpathW, L"%s\\%s", (WCHAR *)dest, infos->filename);
				hDestFile = CreateFileW(destpathW, GENERIC_WRITE, 0, NULL,
						CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
			}else{
				char filenameA[MAX_PATH];
				char destpathA[MAX_PATH];
				UnicodeToAnsi(infos->filename, filenameA, MAX_PATH);
				wsprintfA(destpathA, "%s\\%s", dest, filenameA);
				hDestFile = CreateFileA(destpathA, GENERIC_WRITE, 0, NULL,
						CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
			}
			if ( hDestFile != INVALID_HANDLE_VALUE ){
				DWORD size;

				if ( WriteFile(hDestFile, bin, infos->filesize, &size, NULL) == FALSE ){
					result = SUSIEERROR_INTERNAL;
				}
				CloseHandle(hDestFile);
			}else{
				result = SUSIEERROR_INTERNAL;
			}
			LocalUnlock(himage);
			LocalFree(himage);
		}
		FreeSusieInfoList(sil);
		if ( progressCallback != NULL ) progressCallback(100, 100, lData);
		return result;
	}
	if ( himage != NULL ){
		LocalUnlock(himage);
		LocalFree(himage);
	}
	FreeSusieInfoList(sil);
	if ( progressCallback != NULL ) progressCallback(50, 100, lData);
	return SUSIEERROR_FAULTREAD;
}

extern "C" int __stdcall GetFile(LPCSTR src, LONG_PTR len, LPSTR dest, unsigned int flag, SUSIE_PROGRESS progressCallback, LONG_PTR lData)
{
	return GetFileLocal(src, len, dest, flag, progressCallback, lData, FALSE);
}

extern "C" int __stdcall GetFileW(LPCWSTR src, LONG_PTR len, LPWSTR dest, unsigned int flag, SUSIE_PROGRESS progressCallback, LONG_PTR lData)
{
	return GetFileLocal((const char *)src, len, (char *)dest, flag, progressCallback, lData, TRUE);
}

int __stdcall GetFileInfoLocal(const char *buf, LONG_PTR len, LPCWSTR filename, unsigned int flag, SUSIE_FINFOW *lpInfo, BOOL unicode)
{
	SUSIE_FINFOW *infos;
	SUSIEINFOLIST sil;
	int result;

	result = GetArchiveInfoLocal(buf, len, flag, sil, unicode);
	if ( result != SUSIEERROR_NOERROR ) return result;

	for ( infos = sil.first ; infos->method[0] != '\0' ; infos++ ){
		#if 0 // dir なし
		if ( stricmpW(infos->filename, filename) != 0 ) continue;
		#else // dir あり
		if ( infos->path[0] == '\0' ){
			if ( stricmpW(infos->filename, filename) != 0 ) continue;
		}else{
			int pathlen = wcslen(infos->path);

			if ( memicmp(infos->path, filename, pathlen * sizeof(WCHAR)) != 0 ){
				continue;
			}
			while ( filename[pathlen] == '\\' ) pathlen++;
			if ( stricmpW(infos->filename, filename + pathlen) != 0 ) continue;
		}
		#endif
		*lpInfo = *infos;
		FreeSusieInfoList(sil);
		return SUSIEERROR_NOERROR;
	}
	FreeSusieInfoList(sil);
	return SUSIEERROR_NOTSUPPORT;
}

extern "C" int __stdcall GetFileInfo(LPCSTR buf, LONG_PTR len, LPCSTR filename, unsigned int flag, SUSIE_FINFO *lpInfo)
{
	WCHAR filenameW[SUSIE_PATH_MAX];
	int result;
	SUSIE_FINFOW infosW;

	AnsiToUnicode(filename, filenameW, SUSIE_PATH_MAX);
	filenameW[SUSIE_PATH_MAX - 1] = '\0';
	result = GetFileInfoLocal(buf, len, filenameW, flag, &infosW, FALSE);
	if ( result != SUSIEERROR_NOERROR ) return result;

	memcpy(lpInfo, &infosW, (8 * sizeof(unsigned char)) + (sizeof(ULONG_PTR) * 3) + sizeof(susie_time_t));
	UnicodeToAnsi(infosW.path, lpInfo->path, SUSIE_PATH_MAX);
	lpInfo->path[SUSIE_PATH_MAX - 1] = '\0';
	UnicodeToAnsi(infosW.filename, lpInfo->filename, SUSIE_PATH_MAX);
	lpInfo->filename[SUSIE_PATH_MAX - 1] = '\0';
	lpInfo->crc = infosW.crc;
	return SUSIEERROR_NOERROR;
}

extern "C" int __stdcall GetFileInfoW(LPCWSTR buf, LONG_PTR len, LPCWSTR filename, unsigned int flag, SUSIE_FINFOW *lpInfo)
{
	return GetFileInfoLocal((char *)buf, len, filename, flag, lpInfo, TRUE);
}
