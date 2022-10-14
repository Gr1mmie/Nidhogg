#pragma once
#include "pch.h"

/*
* Description:
* PatchModule is responsible for patching a certain moudle in a certain process.
*
* Parameters:
* @ModuleToPatch [PatchedModule&] -- All the information regarding the Module that needs to be patched.
*
* Returns:
* @status		 [NTSTATUS]		  -- Whether successfuly patched or not.
*/
NTSTATUS PatchModule(PatchedModule* ModuleToPatch) {
	HANDLE hTargetProcess;
	PEPROCESS TargetProcess;
	PEPROCESS CurrentProcess;
	ULONG oldProtection;
	KAPC_STATE state;
	
	PVOID moduleImageBase = NULL;
	NTSTATUS status = STATUS_UNSUCCESSFUL;
	bool success = true;
	LARGE_INTEGER time = { 0 };
	time.QuadPart = -100ll * 10 * 1000;

	// Validate that the required functions are loaded correctly.
	if (!dimGlobals.ZwProtectVirtualMemory || !dimGlobals.MmCopyVirtualMemory)
		return status;

	// Copying the values to local variables before they are unaccesible because of KeStackAttachProcess.
	WCHAR* moduleName = (WCHAR*)ExAllocatePool(PagedPool, (wcslen(ModuleToPatch->ModuleName) + 1) * sizeof(WCHAR));

	if (!moduleName)
		return status;
	memcpy(moduleName, ModuleToPatch->ModuleName, (wcslen(ModuleToPatch->ModuleName) + 1) * sizeof(WCHAR));
	
	CHAR* functionName = (CHAR*)ExAllocatePool(PagedPool, strlen(ModuleToPatch->FunctionName) + 1);

	if (!functionName) {
		ExFreePool(moduleName);
		return status;
	}
	memcpy(functionName, ModuleToPatch->FunctionName, strlen(ModuleToPatch->FunctionName) + 1);

	CHAR* patch = (CHAR*)ExAllocatePool(PagedPool, strlen(ModuleToPatch->Patch));

	if (!patch) {
		ExFreePool(functionName);
		ExFreePool(moduleName);
		return status;
	}
	memcpy(functionName, ModuleToPatch->FunctionName, strlen(ModuleToPatch->Patch));

	// Getting the PEB.
	if (PsLookupProcessByProcessId((HANDLE)ModuleToPatch->Pid, &TargetProcess) != STATUS_SUCCESS)
		return status;

	KeStackAttachProcess(TargetProcess, &state);
	PREALPEB targetPeb = (PREALPEB)dimGlobals.PsGetProcessPeb(TargetProcess);

	if (!targetPeb) {
		KdPrint((DRIVER_PREFIX "Failed to get PEB.\n"));
		goto CleanUp;
	}

	for (int i = 0; !targetPeb->LoaderData && i < 10; i++)
	{
		KeDelayExecutionThread(KernelMode, TRUE, &time);
	}

	if (!targetPeb->LoaderData) {
		KdPrint((DRIVER_PREFIX "Failed to get LDR.\n"));
		goto CleanUp;
	}

	// Getting the module's image base.
	for (PLIST_ENTRY pListEntry = targetPeb->LoaderData->InLoadOrderModuleList.Flink;
		pListEntry != &targetPeb->LoaderData->InLoadOrderModuleList;
		pListEntry = pListEntry->Flink) {

		PLDR_DATA_TABLE_ENTRY pEntry = CONTAINING_RECORD(pListEntry, LDR_DATA_TABLE_ENTRY, InLoadOrderLinks);

		if (_wcsnicmp(pEntry->FullDllName.Buffer, moduleName, pEntry->FullDllName.Length / sizeof(wchar_t) - 4) == 0) {
			moduleImageBase = pEntry->DllBase;
			break;
		}
	}

	if (!moduleImageBase) {
		KdPrint((DRIVER_PREFIX "Failed to get image base.\n"));
		goto CleanUp;
	}

	// Validating module.
	PIMAGE_DOS_HEADER dosHeader = (PIMAGE_DOS_HEADER)moduleImageBase;

	// Checking that the image is valid PE file.
	if (dosHeader->e_magic != IMAGE_DOS_SIGNATURE)
		goto CleanUp;

	PFULL_IMAGE_NT_HEADERS ntHeaders = (PFULL_IMAGE_NT_HEADERS)((PUCHAR)moduleImageBase + dosHeader->e_lfanew);

	if (ntHeaders->Signature != IMAGE_NT_SIGNATURE)
		goto CleanUp;

	IMAGE_OPTIONAL_HEADER optionalHeader = ntHeaders->OptionalHeader;

	if (optionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress == 0)
		goto CleanUp;

	// Iterating the export directory.
	PIMAGE_EXPORT_DIRECTORY exportDirectory = (PIMAGE_EXPORT_DIRECTORY)((PUCHAR)moduleImageBase + optionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress);

	DWORD* addresses = (DWORD*)((PUCHAR)moduleImageBase + exportDirectory->AddressOfFunctions);
	WORD* ordinals = (WORD*)((PUCHAR)moduleImageBase + exportDirectory->AddressOfNameOrdinals);
	DWORD* names = (DWORD*)((PUCHAR)moduleImageBase + exportDirectory->AddressOfNames);

	for (DWORD j = 0; j < exportDirectory->NumberOfNames; j++) {
		if (_stricmp((char*)((PUCHAR)moduleImageBase + names[j]), functionName) == 0) {
			if (ObOpenObjectByPointer(TargetProcess, OBJ_KERNEL_HANDLE, NULL, PROCESS_ALL_ACCESS, *PsProcessType, KernelMode, &hTargetProcess) != STATUS_SUCCESS) {
				KdPrint((DRIVER_PREFIX "Failed to get process to handle.\n"));
				success = false;
				break;
			}

			// auto patchLen = strlen(patch);
			SIZE_T patchLen = 6;
			auto functionAddress = (PVOID)((PUCHAR)moduleImageBase + addresses[ordinals[j]]);

			// Adding write permissions.
			status = dimGlobals.ZwProtectVirtualMemory(hTargetProcess, &functionAddress, &patchLen, PAGE_EXECUTE_READWRITE, &oldProtection);

			if (status != STATUS_SUCCESS) {
				KdPrint((DRIVER_PREFIX "Failed to change protection, (0x%08X).\n", status));
				ZwClose(hTargetProcess);
				success = false;
				break;
			}

			// Patching the function.
			SIZE_T written;

			// status = dimGlobals.MmCopyVirtualMemory(PsGetCurrentProcess(), pmGlobals.ModulesList.Modules[i].Patch, Process, functionAddress, patchLen, KernelMode, &written);
			char testPatch[6] = { 0xB8, 0x57, 0x00, 0x07, 0x80, 0xC3 };
			status = dimGlobals.MmCopyVirtualMemory(PsGetCurrentProcess(), testPatch, TargetProcess, functionAddress, 6, KernelMode, &written);

			if (status != STATUS_SUCCESS) {
				KdPrint((DRIVER_PREFIX "MmCopyVirtualMemory failed status, (0x%08X).\n", status));
				success = false;
			}
			else
				KdPrint((DRIVER_PREFIX "Patched function #5.\n"));

			// Restoring permissions and cleaning up.
			dimGlobals.ZwProtectVirtualMemory(hTargetProcess, &functionAddress, &patchLen, oldProtection, &oldProtection);

			ZwClose(hTargetProcess);
			KdPrint((DRIVER_PREFIX "Cleaned up #6.\n"));
			break;
		}
	}

	if (success)
		status = STATUS_SUCCESS;
CleanUp:
	KeUnstackDetachProcess(&state);
	ExFreePool(moduleName);
	ExFreePool(functionName);
	ExFreePool(patch);
	ObDereferenceObject(TargetProcess);
	return status;
}
