/***************************************************************************
 Copyright 2004 Sebastian Ewert

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

***************************************************************************/

#include "resource.h"
#include "globals.h"
#include <process.h>
#include <commctrl.h>
#ifdef _WIN64
#include "ed2k_hash_cryptapi.h"
#else
#include "ed2k_hash.h"
#endif
#include "sha1_ossl.h"
#include "md5_ossl.h"
#include "sha256_ossl.h"
#include "sha512_ossl.h"
extern "C" {
#include "sha3\KeccakHash.h"
}
#include "crc32c.h"
#include "blake2\blake2.h"
#include "blake3\blake3.h"
#include "CSyncQueue.h"

DWORD WINAPI ThreadProc_Md5Calc(VOID * pParam);
DWORD WINAPI ThreadProc_Sha1Calc(VOID * pParam);
DWORD WINAPI ThreadProc_Sha256Calc(VOID * pParam);
DWORD WINAPI ThreadProc_Sha512Calc(VOID * pParam);
DWORD WINAPI ThreadProc_Ed2kCalc(VOID * pParam);
DWORD WINAPI ThreadProc_CrcCalc(VOID * pParam);
DWORD WINAPI ThreadProc_Sha3_224Calc(VOID * pParam);
DWORD WINAPI ThreadProc_Sha3_256Calc(VOID * pParam);
DWORD WINAPI ThreadProc_Sha3_512Calc(VOID * pParam);
DWORD WINAPI ThreadProc_Crc32cCalc(VOID * pParam);
DWORD WINAPI ThreadProc_Blake2spCalc(VOID * pParam);
DWORD WINAPI ThreadProc_Blake3Calc(VOID * pParam);

// used in UINT __stdcall ThreadProc_Calc(VOID * pParam)
#define SWAPBUFFERS() \
	tempBuffer=readBuffer;\
	dwBytesReadTb=dwBytesReadRb;\
	readBuffer=calcBuffer;\
	dwBytesReadRb=dwBytesReadCb;\
	calcBuffer=tempBuffer;\
	dwBytesReadCb=dwBytesReadTb

typedef DWORD (WINAPI *threadfunc)(VOID * pParam);

threadfunc hash_function[] = {
    ThreadProc_CrcCalc,
    ThreadProc_Md5Calc,
    ThreadProc_Ed2kCalc,
    ThreadProc_Sha1Calc,
    ThreadProc_Sha256Calc,
    ThreadProc_Sha512Calc,
    ThreadProc_Sha3_224Calc,
    ThreadProc_Sha3_256Calc,
    ThreadProc_Sha3_512Calc,
    ThreadProc_Crc32cCalc,
    ThreadProc_Blake2spCalc,
	ThreadProc_Blake3Calc,
};

/*****************************************************************************
UINT __stdcall ThreadProc_Calc(VOID * pParam)
	pParam	: (IN/OUT) THREAD_PARAMS_CALC struct pointer special for this thread

Return Value:
	returns 0

Notes:
- requests jobs from the queue and calculates hashes until the queue is empty
- spawns up to three additional threads, one for each hash value
- performs asynchronous I/O with two buffers -> one buffer is filled while the hash-threads
  work on the other buffer
- if an error occured, GetLastError() is saved in the current pFileinfo->dwError
- what has be calculated is determined by bDoCalculate[HASH_TYPE_CRC32]/bDoCalculate[HASH_TYPE_MD5]/bDoCalculate[HASH_TYPE_ED2K] of the
  current job
*****************************************************************************/
UINT __stdcall ThreadProc_Calc(VOID * pParam)
{
	THREAD_PARAMS_CALC * CONST pthread_params_calc = (THREAD_PARAMS_CALC *)pParam;
	CONST HWND * CONST arrHwnd = pthread_params_calc->arrHwnd;
	SHOWRESULT_PARAMS * CONST pshowresult_params = pthread_params_calc->pshowresult_params;

	BOOL bDoCalculate[NUM_HASH_TYPES];

	QWORD qwStart, qwStop, wqFreq;
	HANDLE hFile;
    UINT uiBufferSize = g_program_options.uiReadBufferSizeKb * 1024;
	bool doUnbufferedReads = g_program_options.bUseUnbufferedReads;
	BYTE *readBuffer = NULL;
	BYTE *calcBuffer = NULL;
	if (doUnbufferedReads) {
		readBuffer = (BYTE *)VirtualAlloc(NULL, uiBufferSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
		calcBuffer = (BYTE *)VirtualAlloc(NULL, uiBufferSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
	} else {
		readBuffer = (BYTE *)malloc(uiBufferSize);
		calcBuffer = (BYTE *)malloc(uiBufferSize);
	}
	BYTE *tempBuffer;
	DWORD readWords[2];
	DWORD *dwBytesReadRb = &readWords[0];
	DWORD *dwBytesReadCb = &readWords[1];
	DWORD *dwBytesReadTb;
	BOOL bSuccess;
	BOOL bFileDone;
	BOOL bAsync;

    HANDLE hEvtThreadGo[NUM_HASH_TYPES];
    HANDLE hEvtThreadReady[NUM_HASH_TYPES];

	HANDLE hEvtReadDone;
	OVERLAPPED olp;
	ZeroMemory(&olp,sizeof(olp));

    HANDLE hThread[NUM_HASH_TYPES];
	
	HANDLE hEvtReadyHandles[NUM_HASH_TYPES];
	DWORD cEvtReadyHandles;

    THREAD_PARAMS_HASHCALC calcParams[NUM_HASH_TYPES];

	lFILEINFO *fileList;
	list<FILEINFO*> finalList;

	if(readBuffer == NULL || calcBuffer == NULL) {
		ShowErrorMsg(arrHwnd[ID_MAIN_WND],GetLastError());
		ExitProcess(1);
	}
	

	// set some UI stuff:
	// - disable action buttons while in thread
	EnableWindowsForThread(arrHwnd, FALSE);

	ShowResult(arrHwnd, NULL, pshowresult_params);
	
	while((fileList = SyncQueue.popQueue()) != NULL) {

        cEvtReadyHandles = 0;

        for(int i=0;i<NUM_HASH_TYPES;i++) {
		    bDoCalculate[i]	= !fileList->bCalculated[i] && fileList->bDoCalculate[i];

            if(bDoCalculate[i]) {
			    fileList->bCalculated[i] = TRUE;
			    hEvtThreadGo[i] = CreateEvent(NULL,FALSE,FALSE,NULL);
			    hEvtThreadReady[i] = CreateEvent(NULL,FALSE,FALSE,NULL);
			    if(hEvtThreadGo[i] == NULL || hEvtThreadReady[i] == NULL) {
				    ShowErrorMsg(arrHwnd[ID_MAIN_WND],GetLastError());
				    ExitProcess(1);
			    }
			    hEvtReadyHandles[cEvtReadyHandles] = hEvtThreadReady[i];
			    cEvtReadyHandles++;
			    calcParams[i].bFileDone = &bFileDone;
			    calcParams[i].hHandleGo = hEvtThreadGo[i];
			    calcParams[i].hHandleReady = hEvtThreadReady[i];
			    calcParams[i].buffer = &calcBuffer;
			    calcParams[i].dwBytesRead = &dwBytesReadCb;
		    }
        }

		hEvtReadDone = CreateEvent(NULL,FALSE,FALSE,NULL);
		if(hEvtReadDone == NULL) {
			ShowErrorMsg(arrHwnd[ID_MAIN_WND],GetLastError());
			ExitProcess(1);
		}

		QueryPerformanceFrequency((LARGE_INTEGER*)&wqFreq);

		if(g_program_options.bEnableQueue && g_pstatus.bHaveComCtrlv6) {
			if(fileList->iGroupId==0)
				InsertGroupIntoListView(arrHwnd[ID_LISTVIEW],fileList);
			else
				RemoveGroupItems(arrHwnd[ID_LISTVIEW],fileList->iGroupId);
        } else {
            ListView_DeleteAllItems(arrHwnd[ID_LISTVIEW]);
        }

		for(list<FILEINFO>::iterator it=fileList->fInfos.begin();it!=fileList->fInfos.end();it++)
		{
			pthread_params_calc->pFileinfo_cur = &(*it);
			pthread_params_calc->qwBytesReadCurFile = 0;
			
			FILEINFO& curFileInfo = (*it);

            bFileDone = TRUE; // assume done until we successfully opened the file

			if ( (curFileInfo.dwError == NO_ERROR) && cEvtReadyHandles > 0)
			{

                DisplayStatusOverview(arrHwnd[ID_EDIT_STATUS]);

				QueryPerformanceCounter((LARGE_INTEGER*) &qwStart);
				DWORD flags = FILE_FLAG_OVERLAPPED | FILE_FLAG_SEQUENTIAL_SCAN;
				if (doUnbufferedReads) {
					flags |= FILE_FLAG_NO_BUFFERING;
				}
				hFile = CreateFile(curFileInfo.szFilename,
						GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, flags, 0);
				if(hFile == INVALID_HANDLE_VALUE) {
					curFileInfo.dwError = GetLastError();
                } else {

				    bFileDone = FALSE;

                    for(int i=0;i<NUM_HASH_TYPES;i++) {
                        if(bDoCalculate[i]) {
                            ResetEvent(hEvtThreadGo[i]);
                            ResetEvent(hEvtThreadReady[i]);
                            calcParams[i].result = &curFileInfo.hashInfo[i].r;
					        hThread[i] = CreateThread(NULL,0,hash_function[i],&calcParams[i],0,NULL);
					        if(hThread[i] == NULL) {
						        ShowErrorMsg(arrHwnd[ID_MAIN_WND],GetLastError());
						        ExitProcess(1);
					        }
                        }
				    }

				    ZeroMemory(&olp,sizeof(olp));
				    olp.hEvent = hEvtReadDone;
				    olp.Offset = 0;
				    olp.OffsetHigh = 0;
				    bSuccess = ReadFile(hFile, readBuffer, uiBufferSize, dwBytesReadRb, &olp);
				    if(!bSuccess && (GetLastError()==ERROR_IO_PENDING))
					    bAsync = TRUE;
				    else
					    bAsync = FALSE;

				    do {
					    if(bAsync)
						    bSuccess = GetOverlappedResult(hFile,&olp,dwBytesReadRb,TRUE);
					    if(!bSuccess && (GetLastError() != ERROR_HANDLE_EOF)) {
						    curFileInfo.dwError = GetLastError();
						    bFileDone = TRUE;
					    }
					    pthread_params_calc->qwBytesReadCurFile  += *dwBytesReadRb; //for progress bar
					    pthread_params_calc->qwBytesReadAllFiles += *dwBytesReadRb;

					    olp.Offset = pthread_params_calc->qwBytesReadCurFile & 0xffffffff;
					    olp.OffsetHigh = (pthread_params_calc->qwBytesReadCurFile >> 32) & 0xffffffff;
    					
					    WaitForMultipleObjects(cEvtReadyHandles,hEvtReadyHandles,TRUE,INFINITE);
					    SWAPBUFFERS();
					    bSuccess = ReadFile(hFile, readBuffer, uiBufferSize, dwBytesReadRb, &olp);
					    if(!bSuccess && (GetLastError()==ERROR_IO_PENDING))
						    bAsync = TRUE;
					    else
						    bAsync = FALSE;

					    if(*dwBytesReadCb < uiBufferSize)
						    bFileDone=TRUE;

                        for(int i=0;i<NUM_HASH_TYPES;i++) {
                            if(bDoCalculate[i])
						        SetEvent(hEvtThreadGo[i]);
                        }

				    } while(!bFileDone && !pthread_params_calc->signalStop);

				    WaitForMultipleObjects(cEvtReadyHandles,hEvtReadyHandles,TRUE,INFINITE);

				    if(hFile != NULL)
					    CloseHandle(hFile);

                    for(int i=0;i<NUM_HASH_TYPES;i++) {
                        if(bDoCalculate[i])
					        CloseHandle(hThread[i]);
                    }

				    QueryPerformanceCounter((LARGE_INTEGER*) &qwStop);
				    curFileInfo.fSeconds = (float)((qwStop - qwStart) / (float)wqFreq);
                }
			}

            curFileInfo.status = InfoToIntValue(&curFileInfo);

			// only add finished files to the listview
            if(bFileDone) {
			    SetFileInfoStrings(&curFileInfo,fileList);

                if(!g_program_options.bHideVerified || curFileInfo.status != STATUS_OK) {
			        InsertItemIntoList(arrHwnd[ID_LISTVIEW], &curFileInfo,fileList);
                }

                SyncQueue.getDoneList();
                SyncQueue.adjustErrorCounters(&curFileInfo,1);
                SyncQueue.releaseDoneList();

			    ShowResult(arrHwnd, &curFileInfo, pshowresult_params);
            }

			// we are stopping, need to remove unfinished file entries from the list and adjust count
            if(pthread_params_calc->signalStop && !pthread_params_calc->signalExit) {
				// if current file is done keep it
                if(bFileDone)
                    it++;
                size_t size_before = fileList->fInfos.size();
                fileList->fInfos.erase(it, fileList->fInfos.end());
                SyncQueue.getDoneList();
                SyncQueue.dwCountTotal -= (DWORD)(size_before - fileList->fInfos.size());
                SyncQueue.releaseDoneList();
            }

            if(pthread_params_calc->signalStop)
                break;
		}

        for(int i=0;i<NUM_HASH_TYPES;i++) {
            if(bDoCalculate[i]) {
		        CloseHandle(hEvtThreadGo[i]);
			    CloseHandle(hEvtThreadReady[i]);
            }
        }

		// if we are stopping remove any open lists from the queue
        if(pthread_params_calc->signalStop) {
            SyncQueue.clearQueue();
			break;
        }

		if(fileList->uiCmdOpts!=CMD_NORMAL) {
			for(list<FILEINFO>::iterator it=fileList->fInfos.begin();it!=fileList->fInfos.end();it++) {
				finalList.push_back(&(*it));
			}
			finalList.sort(ListPointerCompFunction);
			if (fileList->uiCmdOpts >= CMD_NTFS) {
				ActionHashIntoStream(arrHwnd, TRUE, &finalList, fileList->uiCmdOpts - CMD_NTFS);
			}
			else if (fileList->uiCmdOpts >= CMD_NAME) {
				ActionHashIntoFilename(arrHwnd, TRUE, &finalList, fileList->uiCmdOpts - CMD_NAME);
			}
			else if (fileList->uiCmdOpts < CMD_NORMAL) {
				CreateChecksumFiles(arrHwnd, fileList->uiCmdOpts, &finalList);
			}
			finalList.clear();
		}

		// if stopped before finishing any file we can delete the list
        if(fileList->fInfos.size())
		    SyncQueue.addToList(fileList);
        else
            delete fileList;

	}

	// enable action button after thread is done
	if(!pthread_params_calc->signalExit)
		EnableWindowsForThread(arrHwnd, TRUE);

	PostMessage(arrHwnd[ID_MAIN_WND], WM_THREAD_CALC_DONE, 0, 0);
	
	if (doUnbufferedReads) {
		VirtualFree(readBuffer, 0, MEM_RELEASE);
		VirtualFree(calcBuffer, 0, MEM_RELEASE);
	} else {
		free(readBuffer);
		free(calcBuffer);
	}

	_endthreadex( 0 );
	return 0;
}

/*****************************************************************************
UINT __stdcall ThreadProc_FileInfo(VOID * pParam)
	pParam	: (IN/OUT) THREAD_PARAMS_FILEINFO struct pointer special for this thread

Return Value:
returns 1 if something went wrong. Otherwise 0

Notes:
1) In this step directories are expanded and filesizes, found CRC (from filename) are
collected
2) At last it is checked if the first file is an SFV/MD5 file. If so EnterSfvMode/EnterMd5Mode
   is called
3) It sends an application defined window message to signal that is has done its job and
the CRC Thread can start
*****************************************************************************/
UINT __stdcall ThreadProc_FileInfo(VOID * pParam)
{
	THREAD_PARAMS_FILEINFO * thread_params_fileinfo = (THREAD_PARAMS_FILEINFO *)pParam;
	// put thread parameter into (const) locale variable. This might be a bit faster
	CONST HWND * CONST arrHwnd = thread_params_fileinfo->arrHwnd;
	SHOWRESULT_PARAMS * CONST pshowresult_params = thread_params_fileinfo->pshowresult_params;
	lFILEINFO * fileList = thread_params_fileinfo->fileList;

	delete thread_params_fileinfo;

	EnterCriticalSection(&thread_fileinfo_crit);

	if(!g_program_options.bEnableQueue) {
		ClearAllItems(arrHwnd,pshowresult_params);
	}

    // if no forced mode and first file is a hash file enter all hashes mode
    if(fileList->uiCmdOpts == CMD_NORMAL &&
       fileList->fInfos.size() && DetermineHashType(fileList->fInfos.front().szFilename) != MODE_NORMAL)
    {
        fileList->uiCmdOpts = CMD_ALLHASHES;
    }

    if(fileList->uiCmdOpts == CMD_ALLHASHES)
    {
        MakePathsAbsolute(fileList);
        ProcessDirectories(fileList, arrHwnd[ID_EDIT_STATUS], TRUE);
        for(list<FILEINFO>::iterator it=fileList->fInfos.begin();it!=fileList->fInfos.end();it++) {
            lFILEINFO *pHashList = new lFILEINFO;
            pHashList->fInfos.push_back((*it));
            PostProcessList(arrHwnd, pshowresult_params, pHashList);
            SyncQueue.pushQueue(pHashList);
        }
        fileList->fInfos.clear();
    }

	PostProcessList(arrHwnd, pshowresult_params, fileList);

	if(fileList->fInfos.empty()) {
		delete fileList;
	} else {
		SyncQueue.pushQueue(fileList);
	}

	// tell Window Proc that we are done...
	PostMessage(arrHwnd[ID_MAIN_WND], WM_THREAD_FILEINFO_DONE, GetCurrentThreadId(), 0);

	LeaveCriticalSection(&thread_fileinfo_crit);

	_endthreadex( 0 );
	return 0;
}

/*****************************************************************************
void StartFileInfoThread(CONST HWND *arrHwnd, SHOWRESULT_PARAMS *pshowresult_params, lFILEINFO * fileList)
	arrHwnd				: (IN)	   
	pshowresult_params	: (IN/OUT) struct for ShowResult
	fileList			: (IN/OUT) pointer to the job structure that should be processed

Notes:
Helper function to start a fileinfo thread
*****************************************************************************/
UINT StartFileInfoThread(CONST HWND *arrHwnd, SHOWRESULT_PARAMS *pshowresult_params, lFILEINFO * fileList) {
	HANDLE hThread;
	UINT uiThreadID;
	THREAD_PARAMS_FILEINFO *thread_params_fileinfo = new THREAD_PARAMS_FILEINFO;
	thread_params_fileinfo->arrHwnd	= arrHwnd;
	thread_params_fileinfo->pshowresult_params	= pshowresult_params;
	thread_params_fileinfo->fileList = fileList;
	hThread = (HANDLE)_beginthreadex(NULL, 0, ThreadProc_FileInfo, thread_params_fileinfo, 0, &uiThreadID);
	CloseHandle(hThread);
    return uiThreadID;
}

/*****************************************************************************
UINT __stdcall ThreadProc_AcceptPipe(VOID * pParam)
	pParam	: (IN/OUT) THREAD_PARAMS_PIPE struct pointer special for this thread

Return Value:
returns 1 if something went wrong. Otherwise 0

Notes:
reads filenames from the shell extension via Named Pipe, then signals the main
window
*****************************************************************************/
UINT __stdcall ThreadProc_AcceptPipe(VOID * pParam)
{
	THREAD_PARAMS_PIPE * thread_params_pipe = (THREAD_PARAMS_PIPE *)pParam;
	CONST HWND * CONST arrHwnd = thread_params_pipe->arrHwnd;
	lFILEINFO * fileList = thread_params_pipe->fileList;
	delete thread_params_pipe;

	if(!GetDataViaPipe(arrHwnd,fileList)) {
		delete fileList;
		_endthreadex( 0 );
	}

	// tell Window Proc that we are done...
	PostMessage(arrHwnd[ID_MAIN_WND], WM_THREAD_FILEINFO_START, (WPARAM)fileList, 0);

	_endthreadex( 0 );
	return 0;
}

/*****************************************************************************
void StartAcceptPipeThread(CONST HWND *arrHwnd, lFILEINFO * fileList)
	arrHwnd				: (IN)	   
	fileList			: (IN/OUT) pointer to the job structure that should be processed

Notes:
Helper function to start a pipe thread
*****************************************************************************/
void StartAcceptPipeThread(CONST HWND *arrHwnd, lFILEINFO * fileList) {
	HANDLE hThread;
	UINT uiThreadID;
	THREAD_PARAMS_PIPE *thread_params_pipe = new THREAD_PARAMS_PIPE;
	thread_params_pipe->arrHwnd	= arrHwnd;
	thread_params_pipe->fileList = fileList;
	hThread = (HANDLE)_beginthreadex(NULL, 0, ThreadProc_AcceptPipe, thread_params_pipe, 0, &uiThreadID);
	CloseHandle(hThread);
}

/*****************************************************************************
DWORD WINAPI ThreadProc_Md5Calc(VOID * pParam)
	pParam	: (IN/OUT) THREAD_PARAMS_HASHCALC struct pointer special for this thread

Return Value:
	returns 0

Notes:
- initializes the md5 hash calculation and loops through the calculation until
  ThreadProc_Calc signalizes the end of the file
- buffer synchronization is done through hEvtThreadReady and hEvtThreadGo
*****************************************************************************/
DWORD WINAPI ThreadProc_Md5Calc(VOID * pParam)
{
	BYTE ** CONST buffer=((THREAD_PARAMS_HASHCALC *)pParam)->buffer;
	DWORD ** CONST dwBytesRead=((THREAD_PARAMS_HASHCALC *)pParam)->dwBytesRead;
	CONST HANDLE hEvtThreadReady=((THREAD_PARAMS_HASHCALC *)pParam)->hHandleReady;
	CONST HANDLE hEvtThreadGo=((THREAD_PARAMS_HASHCALC *)pParam)->hHandleGo;
	BYTE * CONST result=(BYTE *)((THREAD_PARAMS_HASHCALC *)pParam)->result;
	BOOL * CONST bFileDone=((THREAD_PARAMS_HASHCALC *)pParam)->bFileDone;

	MD5_CTX context;
	MD5_Init(&context);
	do {
		SignalObjectAndWait(hEvtThreadReady,hEvtThreadGo,INFINITE,FALSE);
		MD5_Update(&context, *buffer, **dwBytesRead);
	} while (!(*bFileDone));
	MD5_Final(result,&context);
	SetEvent(hEvtThreadReady);
	return 0;
}

/*****************************************************************************
DWORD WINAPI ThreadProc_Sha1Calc(VOID * pParam)
	pParam	: (IN/OUT) THREAD_PARAMS_HASHCALC struct pointer special for this thread

Return Value:
	returns 0

Notes:
- initializes the sha1 hash calculation and loops through the calculation until
  ThreadProc_Calc signalizes the end of the file
- buffer synchronization is done through hEvtThreadReady and hEvtThreadGo
*****************************************************************************/
DWORD WINAPI ThreadProc_Sha1Calc(VOID * pParam)
{
	BYTE ** CONST buffer=((THREAD_PARAMS_HASHCALC *)pParam)->buffer;
	DWORD ** CONST dwBytesRead=((THREAD_PARAMS_HASHCALC *)pParam)->dwBytesRead;
	CONST HANDLE hEvtThreadReady=((THREAD_PARAMS_HASHCALC *)pParam)->hHandleReady;
	CONST HANDLE hEvtThreadGo=((THREAD_PARAMS_HASHCALC *)pParam)->hHandleGo;
	BYTE * CONST result=(BYTE *)((THREAD_PARAMS_HASHCALC *)pParam)->result;
	BOOL * CONST bFileDone=((THREAD_PARAMS_HASHCALC *)pParam)->bFileDone;

	SHA_CTX context;
	SHA1_Init(&context);
	do {
		SignalObjectAndWait(hEvtThreadReady,hEvtThreadGo,INFINITE,FALSE);
		SHA1_Update(&context, *buffer, **dwBytesRead);
	} while (!(*bFileDone));
	SHA1_Final(result,&context);
	SetEvent(hEvtThreadReady);
	return 0;
}

/*****************************************************************************
DWORD WINAPI ThreadProc_Sha256Calc(VOID * pParam)
	pParam	: (IN/OUT) THREAD_PARAMS_HASHCALC struct pointer special for this thread

Return Value:
	returns 0

Notes:
- initializes the sha256 hash calculation and loops through the calculation until
  ThreadProc_Calc signalizes the end of the file
- buffer synchronization is done through hEvtThreadReady and hEvtThreadGo
*****************************************************************************/
DWORD WINAPI ThreadProc_Sha256Calc(VOID * pParam)
{
	BYTE ** CONST buffer=((THREAD_PARAMS_HASHCALC *)pParam)->buffer;
	DWORD ** CONST dwBytesRead=((THREAD_PARAMS_HASHCALC *)pParam)->dwBytesRead;
	CONST HANDLE hEvtThreadReady=((THREAD_PARAMS_HASHCALC *)pParam)->hHandleReady;
	CONST HANDLE hEvtThreadGo=((THREAD_PARAMS_HASHCALC *)pParam)->hHandleGo;
	BYTE * CONST result=(BYTE *)((THREAD_PARAMS_HASHCALC *)pParam)->result;
	BOOL * CONST bFileDone=((THREAD_PARAMS_HASHCALC *)pParam)->bFileDone;

	SHA256_CTX context;
	SHA256_Init(&context);
	do {
		SignalObjectAndWait(hEvtThreadReady,hEvtThreadGo,INFINITE,FALSE);
		SHA256_Update(&context, *buffer, **dwBytesRead);
	} while (!(*bFileDone));
	SHA256_Final(result,&context);
	SetEvent(hEvtThreadReady);
	return 0;
}

/*****************************************************************************
DWORD WINAPI ThreadProc_Sha512Calc(VOID * pParam)
	pParam	: (IN/OUT) THREAD_PARAMS_HASHCALC struct pointer special for this thread

Return Value:
	returns 0

Notes:
- initializes the sha512 hash calculation and loops through the calculation until
  ThreadProc_Calc signalizes the end of the file
- buffer synchronization is done through hEvtThreadReady and hEvtThreadGo
*****************************************************************************/
DWORD WINAPI ThreadProc_Sha512Calc(VOID * pParam)
{
	BYTE ** CONST buffer=((THREAD_PARAMS_HASHCALC *)pParam)->buffer;
	DWORD ** CONST dwBytesRead=((THREAD_PARAMS_HASHCALC *)pParam)->dwBytesRead;
	CONST HANDLE hEvtThreadReady=((THREAD_PARAMS_HASHCALC *)pParam)->hHandleReady;
	CONST HANDLE hEvtThreadGo=((THREAD_PARAMS_HASHCALC *)pParam)->hHandleGo;
	BYTE * CONST result=(BYTE *)((THREAD_PARAMS_HASHCALC *)pParam)->result;
	BOOL * CONST bFileDone=((THREAD_PARAMS_HASHCALC *)pParam)->bFileDone;

	SHA512_CTX context;
	SHA512_Init(&context);
	do {
		SignalObjectAndWait(hEvtThreadReady,hEvtThreadGo,INFINITE,FALSE);
		SHA512_Update(&context, *buffer, **dwBytesRead);
	} while (!(*bFileDone));
	SHA512_Final(result,&context);
	SetEvent(hEvtThreadReady);
	return 0;
}

/*****************************************************************************
DWORD WINAPI ThreadProc_Sha3_224Calc(VOID * pParam)
	pParam	: (IN/OUT) THREAD_PARAMS_HASHCALC struct pointer special for this thread

Return Value:
	returns 0

Notes:
- initializes the sha3-224 hash calculation and loops through the calculation until
  ThreadProc_Calc signalizes the end of the file
- buffer synchronization is done through hEvtThreadReady and hEvtThreadGo
*****************************************************************************/
DWORD WINAPI ThreadProc_Sha3_224Calc(VOID * pParam)
{
	BYTE ** CONST buffer=((THREAD_PARAMS_HASHCALC *)pParam)->buffer;
	DWORD ** CONST dwBytesRead=((THREAD_PARAMS_HASHCALC *)pParam)->dwBytesRead;
	CONST HANDLE hEvtThreadReady=((THREAD_PARAMS_HASHCALC *)pParam)->hHandleReady;
	CONST HANDLE hEvtThreadGo=((THREAD_PARAMS_HASHCALC *)pParam)->hHandleGo;
	BYTE * CONST result=(BYTE *)((THREAD_PARAMS_HASHCALC *)pParam)->result;
	BOOL * CONST bFileDone=((THREAD_PARAMS_HASHCALC *)pParam)->bFileDone;

	Keccak_HashInstance hashState;
    Keccak_HashInitialize_SHA3_224(&hashState);
	do {
		SignalObjectAndWait(hEvtThreadReady,hEvtThreadGo,INFINITE,FALSE);
		Keccak_HashUpdate(&hashState, *buffer, **dwBytesRead * 8);
	} while (!(*bFileDone));
	Keccak_HashFinal(&hashState, result);
	SetEvent(hEvtThreadReady);
	return 0;
}

/*****************************************************************************
DWORD WINAPI ThreadProc_Sha3_256Calc(VOID * pParam)
	pParam	: (IN/OUT) THREAD_PARAMS_HASHCALC struct pointer special for this thread

Return Value:
	returns 0

Notes:
- initializes the sha3-256 hash calculation and loops through the calculation until
  ThreadProc_Calc signalizes the end of the file
- buffer synchronization is done through hEvtThreadReady and hEvtThreadGo
*****************************************************************************/
DWORD WINAPI ThreadProc_Sha3_256Calc(VOID * pParam)
{
	BYTE ** CONST buffer=((THREAD_PARAMS_HASHCALC *)pParam)->buffer;
	DWORD ** CONST dwBytesRead=((THREAD_PARAMS_HASHCALC *)pParam)->dwBytesRead;
	CONST HANDLE hEvtThreadReady=((THREAD_PARAMS_HASHCALC *)pParam)->hHandleReady;
	CONST HANDLE hEvtThreadGo=((THREAD_PARAMS_HASHCALC *)pParam)->hHandleGo;
	BYTE * CONST result=(BYTE *)((THREAD_PARAMS_HASHCALC *)pParam)->result;
	BOOL * CONST bFileDone=((THREAD_PARAMS_HASHCALC *)pParam)->bFileDone;

	Keccak_HashInstance hashState;
	Keccak_HashInitialize_SHA3_256(&hashState);
	do {
		SignalObjectAndWait(hEvtThreadReady,hEvtThreadGo,INFINITE,FALSE);
		Keccak_HashUpdate(&hashState, *buffer, **dwBytesRead * 8);
	} while (!(*bFileDone));
	Keccak_HashFinal(&hashState, result);
	SetEvent(hEvtThreadReady);
	return 0;
}

/*****************************************************************************
DWORD WINAPI ThreadProc_Sha3_512Calc(VOID * pParam)
	pParam	: (IN/OUT) THREAD_PARAMS_HASHCALC struct pointer special for this thread

Return Value:
	returns 0

Notes:
- initializes the sha3-512 hash calculation and loops through the calculation until
  ThreadProc_Calc signalizes the end of the file
- buffer synchronization is done through hEvtThreadReady and hEvtThreadGo
*****************************************************************************/
DWORD WINAPI ThreadProc_Sha3_512Calc(VOID * pParam)
{
	BYTE ** CONST buffer=((THREAD_PARAMS_HASHCALC *)pParam)->buffer;
	DWORD ** CONST dwBytesRead=((THREAD_PARAMS_HASHCALC *)pParam)->dwBytesRead;
	CONST HANDLE hEvtThreadReady=((THREAD_PARAMS_HASHCALC *)pParam)->hHandleReady;
	CONST HANDLE hEvtThreadGo=((THREAD_PARAMS_HASHCALC *)pParam)->hHandleGo;
	BYTE * CONST result=(BYTE *)((THREAD_PARAMS_HASHCALC *)pParam)->result;
	BOOL * CONST bFileDone=((THREAD_PARAMS_HASHCALC *)pParam)->bFileDone;

	Keccak_HashInstance hashState;
	Keccak_HashInitialize_SHA3_512(&hashState);
	do {
		SignalObjectAndWait(hEvtThreadReady,hEvtThreadGo,INFINITE,FALSE);
		Keccak_HashUpdate(&hashState, *buffer, **dwBytesRead * 8);
	} while (!(*bFileDone));
	Keccak_HashFinal(&hashState, result);
	SetEvent(hEvtThreadReady);
	return 0;
}

/*****************************************************************************
DWORD WINAPI ThreadProc_Ed2kCalc(VOID * pParam)
	pParam	: (IN/OUT) THREAD_PARAMS_HASHCALC struct pointer special for this thread

Return Value:
	returns 0

Notes:
- initializes the ed2k hash calculation and loops through the calculation until
  ThreadProc_Calc signalizes the end of the file
- buffer synchronization is done through hEvtThreadReady and hEvtThreadGo
*****************************************************************************/
DWORD WINAPI ThreadProc_Ed2kCalc(VOID * pParam)
{
	BYTE ** CONST buffer=((THREAD_PARAMS_HASHCALC *)pParam)->buffer;
	DWORD ** CONST dwBytesRead=((THREAD_PARAMS_HASHCALC *)pParam)->dwBytesRead;
	CONST HANDLE hEvtThreadReady=((THREAD_PARAMS_HASHCALC *)pParam)->hHandleReady;
	CONST HANDLE hEvtThreadGo=((THREAD_PARAMS_HASHCALC *)pParam)->hHandleGo;
	BYTE * CONST result=(BYTE *)((THREAD_PARAMS_HASHCALC *)pParam)->result;
	BOOL * CONST bFileDone=((THREAD_PARAMS_HASHCALC *)pParam)->bFileDone;

	CEd2kHash ed2khash;
	ed2khash.restart_calc();
	do {
		SignalObjectAndWait(hEvtThreadReady,hEvtThreadGo,INFINITE,FALSE);
		ed2khash.add_data(*buffer,**dwBytesRead);
	} while (!(*bFileDone));
	ed2khash.finish_calc();
	ed2khash.get_hash(result);
	SetEvent(hEvtThreadReady);
	return 0;
}

#ifdef _WIN64
extern "C" void __fastcall crcCalc(DWORD *pdwCrc32,DWORD *ptrCrc32Table,BYTE *bufferAsm,DWORD dwBytesReadAsm);
#else
extern "C" void crcCalc(DWORD *pdwCrc32,DWORD *ptrCrc32Table,BYTE *bufferAsm,DWORD dwBytesReadAsm);
#endif

/*****************************************************************************
DWORD WINAPI ThreadProc_CrcCalc(VOID * pParam)
	pParam	: (IN/OUT) THREAD_PARAMS_HASHCALC struct pointer special for this thread

Return Value:
	returns 0

Notes:
- initializes the crc hash calculation and loops through the calculation until
  ThreadProc_Calc signalizes the end of the file
- buffer synchronization is done through hEvtThreadReady and hEvtThreadGo
*****************************************************************************/
DWORD WINAPI ThreadProc_CrcCalc(VOID * pParam)
{
	BYTE ** CONST buffer=((THREAD_PARAMS_HASHCALC *)pParam)->buffer;
	DWORD ** CONST dwBytesRead=((THREAD_PARAMS_HASHCALC *)pParam)->dwBytesRead;
	CONST HANDLE hEvtThreadReady=((THREAD_PARAMS_HASHCALC *)pParam)->hHandleReady;
	CONST HANDLE hEvtThreadGo=((THREAD_PARAMS_HASHCALC *)pParam)->hHandleGo;
	DWORD * CONST result=(DWORD *)((THREAD_PARAMS_HASHCALC *)pParam)->result;
	BOOL * CONST bFileDone=((THREAD_PARAMS_HASHCALC *)pParam)->bFileDone;

	DWORD dwCrc32;
	DWORD * CONST pdwCrc32 = &dwCrc32;
	BYTE *bufferAsm;
	DWORD dwBytesReadAsm;

	// Static CRC table; we have a table lookup algorithm
	static CONST DWORD arrdwCrc32Table[256] =
	{
		0x00000000, 0x77073096, 0xEE0E612C, 0x990951BA,
		0x076DC419, 0x706AF48F, 0xE963A535, 0x9E6495A3,
		0x0EDB8832, 0x79DCB8A4, 0xE0D5E91E, 0x97D2D988,
		0x09B64C2B, 0x7EB17CBD, 0xE7B82D07, 0x90BF1D91,
		0x1DB71064, 0x6AB020F2, 0xF3B97148, 0x84BE41DE,
		0x1ADAD47D, 0x6DDDE4EB, 0xF4D4B551, 0x83D385C7,
		0x136C9856, 0x646BA8C0, 0xFD62F97A, 0x8A65C9EC,
		0x14015C4F, 0x63066CD9, 0xFA0F3D63, 0x8D080DF5,
		0x3B6E20C8, 0x4C69105E, 0xD56041E4, 0xA2677172,
		0x3C03E4D1, 0x4B04D447, 0xD20D85FD, 0xA50AB56B,
		0x35B5A8FA, 0x42B2986C, 0xDBBBC9D6, 0xACBCF940,
		0x32D86CE3, 0x45DF5C75, 0xDCD60DCF, 0xABD13D59,
		0x26D930AC, 0x51DE003A, 0xC8D75180, 0xBFD06116,
		0x21B4F4B5, 0x56B3C423, 0xCFBA9599, 0xB8BDA50F,
		0x2802B89E, 0x5F058808, 0xC60CD9B2, 0xB10BE924,
		0x2F6F7C87, 0x58684C11, 0xC1611DAB, 0xB6662D3D,

		0x76DC4190, 0x01DB7106, 0x98D220BC, 0xEFD5102A,
		0x71B18589, 0x06B6B51F, 0x9FBFE4A5, 0xE8B8D433,
		0x7807C9A2, 0x0F00F934, 0x9609A88E, 0xE10E9818,
		0x7F6A0DBB, 0x086D3D2D, 0x91646C97, 0xE6635C01,
		0x6B6B51F4, 0x1C6C6162, 0x856530D8, 0xF262004E,
		0x6C0695ED, 0x1B01A57B, 0x8208F4C1, 0xF50FC457,
		0x65B0D9C6, 0x12B7E950, 0x8BBEB8EA, 0xFCB9887C,
		0x62DD1DDF, 0x15DA2D49, 0x8CD37CF3, 0xFBD44C65,
		0x4DB26158, 0x3AB551CE, 0xA3BC0074, 0xD4BB30E2,
		0x4ADFA541, 0x3DD895D7, 0xA4D1C46D, 0xD3D6F4FB,
		0x4369E96A, 0x346ED9FC, 0xAD678846, 0xDA60B8D0,
		0x44042D73, 0x33031DE5, 0xAA0A4C5F, 0xDD0D7CC9,
		0x5005713C, 0x270241AA, 0xBE0B1010, 0xC90C2086,
		0x5768B525, 0x206F85B3, 0xB966D409, 0xCE61E49F,
		0x5EDEF90E, 0x29D9C998, 0xB0D09822, 0xC7D7A8B4,
		0x59B33D17, 0x2EB40D81, 0xB7BD5C3B, 0xC0BA6CAD,

		0xEDB88320, 0x9ABFB3B6, 0x03B6E20C, 0x74B1D29A,
		0xEAD54739, 0x9DD277AF, 0x04DB2615, 0x73DC1683,
		0xE3630B12, 0x94643B84, 0x0D6D6A3E, 0x7A6A5AA8,
		0xE40ECF0B, 0x9309FF9D, 0x0A00AE27, 0x7D079EB1,
		0xF00F9344, 0x8708A3D2, 0x1E01F268, 0x6906C2FE,
		0xF762575D, 0x806567CB, 0x196C3671, 0x6E6B06E7,
		0xFED41B76, 0x89D32BE0, 0x10DA7A5A, 0x67DD4ACC,
		0xF9B9DF6F, 0x8EBEEFF9, 0x17B7BE43, 0x60B08ED5,
		0xD6D6A3E8, 0xA1D1937E, 0x38D8C2C4, 0x4FDFF252,
		0xD1BB67F1, 0xA6BC5767, 0x3FB506DD, 0x48B2364B,
		0xD80D2BDA, 0xAF0A1B4C, 0x36034AF6, 0x41047A60,
		0xDF60EFC3, 0xA867DF55, 0x316E8EEF, 0x4669BE79,
		0xCB61B38C, 0xBC66831A, 0x256FD2A0, 0x5268E236,
		0xCC0C7795, 0xBB0B4703, 0x220216B9, 0x5505262F,
		0xC5BA3BBE, 0xB2BD0B28, 0x2BB45A92, 0x5CB36A04,
		0xC2D7FFA7, 0xB5D0CF31, 0x2CD99E8B, 0x5BDEAE1D,

		0x9B64C2B0, 0xEC63F226, 0x756AA39C, 0x026D930A,
		0x9C0906A9, 0xEB0E363F, 0x72076785, 0x05005713,
		0x95BF4A82, 0xE2B87A14, 0x7BB12BAE, 0x0CB61B38,
		0x92D28E9B, 0xE5D5BE0D, 0x7CDCEFB7, 0x0BDBDF21,
		0x86D3D2D4, 0xF1D4E242, 0x68DDB3F8, 0x1FDA836E,
		0x81BE16CD, 0xF6B9265B, 0x6FB077E1, 0x18B74777,
		0x88085AE6, 0xFF0F6A70, 0x66063BCA, 0x11010B5C,
		0x8F659EFF, 0xF862AE69, 0x616BFFD3, 0x166CCF45,
		0xA00AE278, 0xD70DD2EE, 0x4E048354, 0x3903B3C2,
		0xA7672661, 0xD06016F7, 0x4969474D, 0x3E6E77DB,
		0xAED16A4A, 0xD9D65ADC, 0x40DF0B66, 0x37D83BF0,
		0xA9BCAE53, 0xDEBB9EC5, 0x47B2CF7F, 0x30B5FFE9,
		0xBDBDF21C, 0xCABAC28A, 0x53B39330, 0x24B4A3A6,
		0xBAD03605, 0xCDD70693, 0x54DE5729, 0x23D967BF,
		0xB3667A2E, 0xC4614AB8, 0x5D681B02, 0x2A6F2B94,
		0xB40BBE37, 0xC30C8EA1, 0x5A05DF1B, 0x2D02EF8D,
	};
	// There is a bug in the Microsoft compilers where inline assembly
	// code cannot access static member variables.  This is a work around
	// for that bug.  For more info see Knowledgebase article Q88092
	CONST VOID * CONST ptrCrc32Table = &arrdwCrc32Table;
	dwCrc32 = 0xFFFFFFFF;

	do {
		SignalObjectAndWait(hEvtThreadReady,hEvtThreadGo,INFINITE,FALSE);
		
		bufferAsm = *buffer;
		dwBytesReadAsm = **dwBytesRead;

		if(dwBytesReadAsm==0) continue;

		// Register use:
		//		eax - CRC32 value
		//		ebx - a lot of things
		//		ecx - CRC32 value
		//		edx - address of end of buffer
		//		esi - address of start of buffer
		//		edi - CRC32 table
		//
		// assembly part by Brian Friesen
		crcCalc(pdwCrc32,(DWORD *)&arrdwCrc32Table,bufferAsm,dwBytesReadAsm);

	} while (!(*bFileDone));
	*result = ~dwCrc32;
	SetEvent(hEvtThreadReady);
	return 0;
}

DWORD WINAPI ThreadProc_Crc32cCalc(VOID * pParam)
{
	BYTE ** CONST buffer=((THREAD_PARAMS_HASHCALC *)pParam)->buffer;
	DWORD ** CONST dwBytesRead=((THREAD_PARAMS_HASHCALC *)pParam)->dwBytesRead;
	CONST HANDLE hEvtThreadReady=((THREAD_PARAMS_HASHCALC *)pParam)->hHandleReady;
	CONST HANDLE hEvtThreadGo=((THREAD_PARAMS_HASHCALC *)pParam)->hHandleGo;
	DWORD * CONST result=(DWORD *)((THREAD_PARAMS_HASHCALC *)pParam)->result;
	BOOL * CONST bFileDone=((THREAD_PARAMS_HASHCALC *)pParam)->bFileDone;

    __crc32_init();

    DWORD dwCrc32c = 0;

	do {
		SignalObjectAndWait(hEvtThreadReady,hEvtThreadGo,INFINITE,FALSE);
		dwCrc32c = crc32c_append(dwCrc32c, *buffer, **dwBytesRead);
	} while (!(*bFileDone));
	*result = dwCrc32c;
	SetEvent(hEvtThreadReady);
	return 0;
}

DWORD WINAPI ThreadProc_Blake2spCalc(VOID * pParam)
{
	BYTE ** CONST buffer=((THREAD_PARAMS_HASHCALC *)pParam)->buffer;
	DWORD ** CONST dwBytesRead=((THREAD_PARAMS_HASHCALC *)pParam)->dwBytesRead;
	CONST HANDLE hEvtThreadReady=((THREAD_PARAMS_HASHCALC *)pParam)->hHandleReady;
	CONST HANDLE hEvtThreadGo=((THREAD_PARAMS_HASHCALC *)pParam)->hHandleGo;
	BYTE * CONST result=(BYTE *)((THREAD_PARAMS_HASHCALC *)pParam)->result;
	BOOL * CONST bFileDone=((THREAD_PARAMS_HASHCALC *)pParam)->bFileDone;

    blake2sp_state state;

    blake2sp_init( &state, 32 );

	do {
		SignalObjectAndWait(hEvtThreadReady,hEvtThreadGo,INFINITE,FALSE);
        blake2sp_update( &state, *buffer, **dwBytesRead );
	} while (!(*bFileDone));

	blake2sp_final( &state, result, 32 );

	SetEvent(hEvtThreadReady);
	return 0;
}

DWORD WINAPI ThreadProc_Blake3Calc(VOID * pParam)
{
	BYTE ** CONST buffer = ((THREAD_PARAMS_HASHCALC *)pParam)->buffer;
	DWORD ** CONST dwBytesRead = ((THREAD_PARAMS_HASHCALC *)pParam)->dwBytesRead;
	CONST HANDLE hEvtThreadReady = ((THREAD_PARAMS_HASHCALC *)pParam)->hHandleReady;
	CONST HANDLE hEvtThreadGo = ((THREAD_PARAMS_HASHCALC *)pParam)->hHandleGo;
	BYTE * CONST result = (BYTE *)((THREAD_PARAMS_HASHCALC *)pParam)->result;
	BOOL * CONST bFileDone = ((THREAD_PARAMS_HASHCALC *)pParam)->bFileDone;

	blake3_hasher hasher;
	blake3_hasher_init(&hasher);

	do {
		SignalObjectAndWait(hEvtThreadReady, hEvtThreadGo, INFINITE, FALSE);
		blake3_hasher_update(&hasher, *buffer, **dwBytesRead);
	} while (!(*bFileDone));

	blake3_hasher_finalize(&hasher, result, BLAKE3_OUT_LEN);

	SetEvent(hEvtThreadReady);
	return 0;
}
