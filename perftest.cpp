/*************************************************************************/
/*				Copyright (c) 2000-2016 NT Kernel Resources.		     */
/*                           All Rights Reserved.                        */
/*                          http://www.ntkernel.com                      */
/*                           ndisrd@ntkernel.com                         */
/*                                                                       */
/* Module Name:  PerfTest.cpp                                            */
/*                                                                       */
/* Abstract: Defines the entry point for the console application         */
/*                                                                       */
/*************************************************************************/

#include "stdafx.h"

//
// Maximum number of packets to fetch from the driver per single operation
//
#define MAX_PACKET_CHUNK 256

//
// Some global definitions
//
TCP_AdapterList				g_AdList;
DWORD						g_iIndex;
CNdisApi					g_api;
HANDLE						g_hEvent;
BOOLEAN						g_bIsRunning = TRUE;
volatile unsigned long long	g_llPacketFiltered = 0;
volatile unsigned			g_dwReadOps = 0;

// 
// Working thread routine. Started one thread per CPU core
//
unsigned __stdcall WorkingThread(void * index)
{	
	
	
	PETH_M_REQUEST		ReadRequest;
	PETH_M_REQUEST		ToMstcpRequest;
	PETH_M_REQUEST		ToAdapterRequest;
	INTERMEDIATE_BUFFER PacketBuffer[MAX_PACKET_CHUNK];
	unsigned			dwMaxRead = 0;
	
	ULONG_PTR			dwThreadIndex = reinterpret_cast<ULONG_PTR>(index);

	printf("Thread: %I64d started\n", (ULONGLONG)dwThreadIndex);
	
	//
	// Initialize Request
	//
	ReadRequest = (PETH_M_REQUEST)malloc(sizeof(ETH_M_REQUEST) +
		sizeof(NDISRD_ETH_Packet)*(MAX_PACKET_CHUNK - 1));
	ToMstcpRequest = (PETH_M_REQUEST)malloc(sizeof(ETH_M_REQUEST) +
		sizeof(NDISRD_ETH_Packet)*(MAX_PACKET_CHUNK - 1));
	ToAdapterRequest = (PETH_M_REQUEST)malloc(sizeof(ETH_M_REQUEST) +
		sizeof(NDISRD_ETH_Packet)*(MAX_PACKET_CHUNK - 1));

	ZeroMemory(ReadRequest, sizeof(ETH_M_REQUEST) +
		sizeof(NDISRD_ETH_Packet)*(MAX_PACKET_CHUNK - 1));
	ZeroMemory(ToMstcpRequest, sizeof(ETH_M_REQUEST) +
		sizeof(NDISRD_ETH_Packet)*(MAX_PACKET_CHUNK - 1));
	ZeroMemory(ToAdapterRequest, sizeof(ETH_M_REQUEST) +
		sizeof(NDISRD_ETH_Packet)*(MAX_PACKET_CHUNK - 1));

	ZeroMemory(&PacketBuffer, sizeof(INTERMEDIATE_BUFFER)*MAX_PACKET_CHUNK);
	ReadRequest->hAdapterHandle = (HANDLE)g_AdList.m_nAdapterHandle[g_iIndex];
	ToMstcpRequest->hAdapterHandle = (HANDLE)g_AdList.m_nAdapterHandle[g_iIndex];
	ToAdapterRequest->hAdapterHandle = (HANDLE)g_AdList.m_nAdapterHandle[g_iIndex];
	ReadRequest->dwPacketsNumber = MAX_PACKET_CHUNK;

	for (unsigned i = 0; i < MAX_PACKET_CHUNK; ++i)
	{
		ReadRequest->EthPacket[i].Buffer = &PacketBuffer[i];
	}

	while (g_bIsRunning)
	{
		WaitForSingleObject(g_hEvent, INFINITE);
		
		// Reset event, as we don't need to wake up all working threads at once

		if (g_bIsRunning)
			ResetEvent(g_hEvent);

		// Start reading packet from the driver

		while (g_api.ReadPackets(ReadRequest))
		{
			if (ReadRequest->dwPacketsSuccess > dwMaxRead)
			{
				dwMaxRead = ReadRequest->dwPacketsSuccess;

				printf(
					"Thread: %I64d New read operation maximum %d packets from driver\n",
					(ULONGLONG)dwThreadIndex,
					dwMaxRead
					);
			}

			for (unsigned i = 0; i < ReadRequest->dwPacketsSuccess; ++i)
			{
				InterlockedIncrement(&g_llPacketFiltered);

				if (PacketBuffer[i].m_dwDeviceFlags == PACKET_FLAG_ON_SEND)
				{
					ToAdapterRequest->EthPacket[ToAdapterRequest->dwPacketsNumber].Buffer = &PacketBuffer[i];
					ToAdapterRequest->dwPacketsNumber++;
				}
				else
				{
					ToMstcpRequest->EthPacket[ToMstcpRequest->dwPacketsNumber].Buffer = &PacketBuffer[i];
					ToMstcpRequest->dwPacketsNumber++;
				}
			}

			if (ToAdapterRequest->dwPacketsNumber)
			{
				g_api.SendPacketsToAdapter(ToAdapterRequest);
				ToAdapterRequest->dwPacketsNumber = 0;
				InterlockedIncrement(&g_dwReadOps);
			}

			if (ToMstcpRequest->dwPacketsNumber)
			{
				g_api.SendPacketsToMstcp(ToMstcpRequest);
				ToMstcpRequest->dwPacketsNumber = 0;
			}

			ReadRequest->dwPacketsSuccess = 0;
		}
	}

	free(ReadRequest);
	free(ToMstcpRequest);
	free(ToAdapterRequest);

	return 0;
}

int main(int argc, char* argv[])
{
	if (argc < 3)
	{
		printf ("Command line syntax:\
				\n\tPerfTest.exe index threads\
				\n\tindex - network interface index.\
				\n\tthreads - number of working threads.\
				\n\tYou can use ListAdapters to determine correct index.\n");

		return 0;
	}

	g_iIndex = atoi(argv[1]) - 1;
	unsigned concurentThreadsSupported = atoi(argv[2]);

	if(!g_api.IsDriverLoaded())
	{
		printf ("Driver not installed on this system of failed to load.\n");
		return 0;
	}
	
	g_api.GetTcpipBoundAdaptersInfo ( &g_AdList );

	if ( g_iIndex + 1 > g_AdList.m_nAdapterCount )
	{
		printf("There is no network interface with such index on this system.\n");
		return 0;
	}
	
	ADAPTER_MODE Mode;

	Mode.dwFlags = MSTCP_FLAG_SENT_TUNNEL|MSTCP_FLAG_RECV_TUNNEL;
	Mode.hAdapterHandle = (HANDLE)g_AdList.m_nAdapterHandle[g_iIndex];

	// Create notification event
	g_hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);

	// Set event for helper driver
	if ((!g_hEvent) || 
		(!g_api.SetPacketEvent((HANDLE)g_AdList.m_nAdapterHandle[g_iIndex], g_hEvent)))
	{
		printf ("Failed to create notification event or set it for driver.\n");
		return 0;
	}

	g_api.SetAdapterMode(&Mode);

	HANDLE* phThreads = new HANDLE[concurentThreadsSupported];

	for (unsigned i = 0; i < concurentThreadsSupported; ++i)
	{
		phThreads[i] = 
			(HANDLE)_beginthreadex (	
				NULL,
				0,
				WorkingThread,
				reinterpret_cast<void*>((ULONG_PTR)i),
				0,
				NULL
				);
	}

	printf("Press any key to stop packet filtering... \n");

	_getch();

	g_bIsRunning = FALSE;

	SetEvent(g_hEvent);

	WaitForMultipleObjects(concurentThreadsSupported, phThreads, TRUE, INFINITE);

	for (unsigned i = 0; i < concurentThreadsSupported; ++i)
	{
		CloseHandle(phThreads[i]);
	}

	printf(
		"Filtered %I64d packets read in %d operations. Packets per read average:%I64d \n ", 
		g_llPacketFiltered, 
		g_dwReadOps, 
		g_llPacketFiltered / ++g_dwReadOps
	);

	//
	// Although we exit application and all resources will be cleaned up
	// automatically let's release everything before we exit
	//
	Mode.dwFlags = 0;
	Mode.hAdapterHandle = (HANDLE)g_AdList.m_nAdapterHandle[g_iIndex];

	// Set NULL event to release previously set event object
	g_api.SetPacketEvent(g_AdList.m_nAdapterHandle[g_iIndex], NULL);

	// Close Event
	if (g_hEvent)
		CloseHandle(g_hEvent);

	// Set default adapter mode
	g_api.SetAdapterMode(&Mode);

	// Empty adapter packets queue
	g_api.FlushAdapterPacketQueue(g_AdList.m_nAdapterHandle[g_iIndex]);

	return 0;
}

