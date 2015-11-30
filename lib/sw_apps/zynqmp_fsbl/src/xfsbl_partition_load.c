/******************************************************************************
*
* Copyright (C) 2015 Xilinx, Inc.  All rights reserved.
*
* Permission is hereby granted, free of charge, to any person obtaining a copy
* of this software and associated documentation files (the "Software"), to deal
* in the Software without restriction, including without limitation the rights
* to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
* copies of the Software, and to permit persons to whom the Software is
* furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in
* all copies or substantial portions of the Software.
*
* Use of the Software is limited solely to applications:
* (a) running on a Xilinx device, or
* (b) that interact with a Xilinx device through a bus or interconnect.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
* XILINX  BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
* WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF
* OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
* SOFTWARE.
*
* Except as contained in this notice, the name of the Xilinx shall not be used
* in advertising or otherwise to promote the sale, use or other dealings in
* this Software without prior written authorization from Xilinx.
*
******************************************************************************/

/*****************************************************************************/
/**
*
* @file xfsbl_partition_load.c
*
* This is the file which contains partition load code for the FSBL.
*
* <pre>
* MODIFICATION HISTORY:
*
* Ver   Who  Date        Changes
* ----- ---- -------- -------------------------------------------------------
* 1.00  kc   10/21/13 Initial release
*
* </pre>
*
* @note
*
******************************************************************************/

/***************************** Include Files *********************************/
#include "xil_cache.h"
#include "xfsbl_hw.h"
#include "xfsbl_main.h"
#include "xfsbl_image_header.h"
#include "xfsbl_hooks.h"
#include "xfsbl_authentication.h"
#include "xfsbl_bs.h"
/************************** Constant Definitions *****************************/

/**************************** Type Definitions *******************************/

/***************** Macros (Inline Functions) Definitions *********************/
#define XFSBL_IVT_LENGTH	(0x8U)

/**
 * Different Memory types
 */
#define XFSBL_R5_0_TCM		(0x1U)
#define XFSBL_R5_1_TCM		(0x2U)
#define XFSBL_R5_L_TCM		(0x3U)

/************************** Function Prototypes ******************************/
static u32 XFsbl_PartitionHeaderValidation(XFsblPs * FsblInstancePtr,
		u32 PartitionNum);
static u32 XFsbl_PartitionCopy(XFsblPs * FsblInstancePtr, u32 PartitionNum);
static u32 XFsbl_PartitionValidation(XFsblPs * FsblInstancePtr,
		u32 PartitionNum);
static u32 XFsbl_CheckHandoffCpu (XFsblPs * FsblInstancePtr,
		u32 DestinationCpu);
static u32 XFsbl_ConfigureMemory(u32 RunningCpu, u32 DestinationCpu,
		u64 Address, u32 Length);
void XFsbl_EccInitialize(u32 Address, u32 Length);
u32 XFsbl_GetLoadAddress(u32 DestinationCpu, PTRSIZE * LoadAddressPtr,
		u32 Length);
static void XFsbl_CheckPmuFw(XFsblPs * FsblInstancePtr, u32 PartitionNum);

/************************** Variable Definitions *****************************/
static int IsR50TcmEccInitialized = FALSE;
static int IsR51TcmEccInitialized = FALSE;
static int IsR5LTcmEccInitialized = FALSE;

u8 TcmVectorArray[32];
u32 TcmSkipLength=0U;
PTRSIZE TcmSkipAddress=0U;
#ifdef XFSBL_AES
static XSecure_Aes SecureAes;
#endif
/*****************************************************************************/
/**
 * This function loads the partition
 *
 * @param	FsblInstancePtr is pointer to the XFsbl Instance
 *
 * @param	PartitionNum is the partition number in the image to be loaded
 *
 * @return	returns the error codes described in xfsbl_error.h on any error
 * 			returns XFSBL_SUCCESS on success
 *
 *****************************************************************************/
u32 XFsbl_PartitionLoad(XFsblPs * FsblInstancePtr, u32 PartitionNum)
{
	u32 Status=XFSBL_SUCCESS;

#ifdef XFSBL_WDT_PRESENT
	/* Restart WDT as partition copy can take more time */
	XFsbl_RestartWdt();
#endif

	/**
	 * Load and validate the partition
	 */

	/**
	 * Partition Header Validation
	 */
	Status = XFsbl_PartitionHeaderValidation(FsblInstancePtr, PartitionNum);

	/**
	 * FSBL is not partition owner and skip this partition
	 */
	if (Status == XFSBL_SUCCESS_NOT_PARTITION_OWNER)
	{
		Status = XFSBL_SUCCESS;
		goto END;
	} else if (XFSBL_SUCCESS != Status)
	{
		goto END;
	} else {
		/**
		 *  for MISRA C compliance
		 */
	}

	/**
	 * Partition Copy
	 */
	Status = XFsbl_PartitionCopy(FsblInstancePtr, PartitionNum);
	if (XFSBL_SUCCESS != Status)
	{
		goto END;
	}

	/**
	 * Partition Validation
	 */
	Status = XFsbl_PartitionValidation(FsblInstancePtr, PartitionNum);
	if (XFSBL_SUCCESS != Status)
	{
		goto END;
	}

	/* Check if PMU FW load is done and handoff it to Microblaze */
	XFsbl_CheckPmuFw(FsblInstancePtr, PartitionNum);

END:
	return Status;
}

/*****************************************************************************/
/**
 * This function validates the partition header
 *
 * @param	FsblInstancePtr is pointer to the XFsbl Instance
 *
 * @param	PartitionNum is the partition number in the image to be loaded
 *
 * @return	returns the error codes described in xfsbl_error.h on any error
 * 			returns XFSBL_SUCCESS on success
 *****************************************************************************/
static u32 XFsbl_PartitionHeaderValidation(XFsblPs * FsblInstancePtr,
		u32 PartitionNum)
{
	u32 Status=XFSBL_SUCCESS;
	XFsblPs_PartitionHeader * PartitionHeader;

	/**
	 * Assign the partition header to local variable
	 */
	PartitionHeader =
		&FsblInstancePtr->ImageHeader.PartitionHeader[PartitionNum];

	/**
	 * Check the check sum of the partition header
	 */
	Status = XFsbl_ValidateChecksum( (u32 *)PartitionHeader, XIH_PH_LEN/4U);
	if (XFSBL_SUCCESS != Status)
	{
		Status = XFSBL_ERROR_PH_CHECKSUM_FAILED;
		XFsbl_Printf(DEBUG_GENERAL,
			"XFSBL_ERROR_PH_CHECKSUM_FAILED\n\r");
		goto END;
	}

	/**
	 * Check if partition belongs to FSBL
	 */
	if (XFsbl_GetPartitionOwner(PartitionHeader) !=
			XIH_PH_ATTRB_PART_OWNER_FSBL)
	{
		/**
		 * If the partition doesn't belong to FSBL, skip the partition
		 */
		XFsbl_Printf(DEBUG_GENERAL,"Skipping the Partition 0x%0lx\n",
				PartitionNum);
		Status = XFSBL_SUCCESS_NOT_PARTITION_OWNER;
		goto END;
	}

	/**
	 * Validate the fields of partition
	 */
	Status = XFsbl_ValidatePartitionHeader(
			PartitionHeader, FsblInstancePtr->ProcessorID);
	if (XFSBL_SUCCESS != Status)
	{
		goto END;
	}

END:
	return Status;
}

/****************************************************************************/
/**
 * This function is used to check whether cpu has handoff address stored
 * in the handoff structure
 *
 * @param FsblInstancePtr is pointer to the XFsbl Instance
 *
 * @param DestinationCpu is the cpu which needs to be checked
 *
 * @return
 * 		- XFSBL_SUCCESS if cpu handoff address is not present
 * 		- XFSBL_FAILURE if cpu handoff address is present
 *
 * @note
 *
 *****************************************************************************/
static u32 XFsbl_CheckHandoffCpu (XFsblPs * FsblInstancePtr,
						u32 DestinationCpu)
{
	u32 ValidHandoffCpuNo=0U;
	u32 Status=XFSBL_SUCCESS;
	u32 Index=0U;
	u32 CpuId=0U;


	ValidHandoffCpuNo = FsblInstancePtr->HandoffCpuNo;

	for (Index=0U;Index<ValidHandoffCpuNo;Index++)
	{
		CpuId = FsblInstancePtr->HandoffValues[Index].CpuSettings &
			XIH_PH_ATTRB_DEST_CPU_MASK;
		if (CpuId == DestinationCpu)
		{
			Status = XFSBL_FAILURE;
			break;
		}
	}

	return Status;
}

/*****************************************************************************/
/**
 * This function checks the power state and reset for the memory type
 * and release the reset if required
 *
 * @param	MemoryType is the memory to be checked
 * 			- XFSBL_R5_0_TCM
 * 			- XFSBL_R5_1_TCM
 *				(to be added)
 *			- XFSBL_R5_0_TCMA
 *			- XFSBL_R5_0_TCMB
 *			- XFSBL_PS_DDR
 *			- XFSBL_PL_DDR
 *
 * @return	none
 *****************************************************************************/
static u32 XFsbl_PowerUpMemory(u32 MemoryType)
{
	u32 RegValue;
	u32 Status = XFSBL_SUCCESS;
	u32 PwrStateMask;

	/**
	 * Check the power status of the memory
	 * Power up if required
	 *
	 * Release the reset of the memory if present
	 */
	switch (MemoryType)
	{
		case XFSBL_R5_0_TCM:
		{

			PwrStateMask = PMU_GLOBAL_PWR_STATE_R5_0_MASK |
					PMU_GLOBAL_PWR_STATE_TCM0A_MASK |
					PMU_GLOBAL_PWR_STATE_TCM0B_MASK;

			Status = XFsbl_PowerUpIsland(PwrStateMask);

			if (Status != XFSBL_SUCCESS) {
				Status = XFSBL_ERROR_R5_0_TCM_POWER_UP;
				XFsbl_Printf(DEBUG_GENERAL, "XFSBL_ERROR_R5_0_TCM_POWER_UP\r\n");
				goto END;
			}

			/**
			 * To access TCM,
			 * 	Release reset to R5 and enable the clk
			 * 	R5 is under halt state
			 *
			 * 	If R5 are out of reset and clk is enabled so doing
			 * 	again is no issue. R5 might be under running state
			 */

			/**
			 * Place R5, TCM in split mode
			 */
			RegValue = XFsbl_In32(RPU_RPU_GLBL_CNTL);
			RegValue |= RPU_RPU_GLBL_CNTL_SLSPLIT_MASK;
			RegValue &= ~(RPU_RPU_GLBL_CNTL_TCM_COMB_MASK);
			RegValue &= ~(RPU_RPU_GLBL_CNTL_SLCLAMP_MASK);
			XFsbl_Out32(RPU_RPU_GLBL_CNTL, RegValue);

			/**
			 * Place R5-0 in HALT state
			 */
			RegValue = XFsbl_In32(RPU_RPU_0_CFG);
			RegValue &= ~(RPU_RPU_0_CFG_NCPUHALT_MASK);
			XFsbl_Out32(RPU_RPU_0_CFG, RegValue);

			/**
			 *  Enable the clock
			 */
			RegValue = XFsbl_In32(CRL_APB_CPU_R5_CTRL);
			RegValue |= CRL_APB_CPU_R5_CTRL_CLKACT_MASK;
			XFsbl_Out32(CRL_APB_CPU_R5_CTRL, RegValue);

			/**
			 * Provide some delay,
			 * so that clock propogates properly.
			 */
			(void)usleep(0x50U);

			/**
			 * Release reset to R5-0
			 */
			RegValue = XFsbl_In32(CRL_APB_RST_LPD_TOP);
			RegValue &= ~(CRL_APB_RST_LPD_TOP_RPU_R50_RESET_MASK);
			RegValue &= ~(CRL_APB_RST_LPD_TOP_RPU_AMBA_RESET_MASK);
			XFsbl_Out32(CRL_APB_RST_LPD_TOP, RegValue);
		} break;

		case XFSBL_R5_1_TCM:
		{

			PwrStateMask = PMU_GLOBAL_PWR_STATE_R5_1_MASK |
					PMU_GLOBAL_PWR_STATE_TCM1A_MASK |
					PMU_GLOBAL_PWR_STATE_TCM1B_MASK;

			Status = XFsbl_PowerUpIsland(PwrStateMask);

			if (Status != XFSBL_SUCCESS) {
				Status = XFSBL_ERROR_R5_1_TCM_POWER_UP;
				XFsbl_Printf(DEBUG_GENERAL, "XFSBL_ERROR_R5_1_TCM_POWER_UP\r\n");
				goto END;
			}

			/**
			 * Place R5 in split mode
			 */
			RegValue = XFsbl_In32(RPU_RPU_GLBL_CNTL);
			RegValue |= RPU_RPU_GLBL_CNTL_SLSPLIT_MASK;
			RegValue &= ~(RPU_RPU_GLBL_CNTL_TCM_COMB_MASK);
			RegValue &= ~(RPU_RPU_GLBL_CNTL_SLCLAMP_MASK);
			XFsbl_Out32(RPU_RPU_GLBL_CNTL, RegValue);

			/**
			 * Place R5-1 in HALT state
			 */
			RegValue = XFsbl_In32(RPU_RPU_1_CFG);
			RegValue &= ~(RPU_RPU_1_CFG_NCPUHALT_MASK);
			XFsbl_Out32(RPU_RPU_1_CFG, RegValue);

			/**
			 *  Enable the clock
			 */
			RegValue = XFsbl_In32(CRL_APB_CPU_R5_CTRL);
			RegValue |= CRL_APB_CPU_R5_CTRL_CLKACT_MASK;
			XFsbl_Out32(CRL_APB_CPU_R5_CTRL, RegValue);

			/**
			 * Provide some delay,
			 * so that clock propogates properly.
			 */
			(void )usleep(0x50U);

			/**
			 * Release reset to R5-1
			 */
			RegValue = XFsbl_In32(CRL_APB_RST_LPD_TOP);
			RegValue &= ~(CRL_APB_RST_LPD_TOP_RPU_R51_RESET_MASK);
			RegValue &= ~(CRL_APB_RST_LPD_TOP_RPU_AMBA_RESET_MASK);
			XFsbl_Out32(CRL_APB_RST_LPD_TOP, RegValue);
		} break;

		case XFSBL_R5_L_TCM:
		{


			PwrStateMask = PMU_GLOBAL_PWR_STATE_R5_0_MASK |
					PMU_GLOBAL_PWR_STATE_TCM0A_MASK |
					PMU_GLOBAL_PWR_STATE_TCM0B_MASK |
					PMU_GLOBAL_PWR_STATE_TCM1A_MASK |
					PMU_GLOBAL_PWR_STATE_TCM1B_MASK;

			Status = XFsbl_PowerUpIsland(PwrStateMask);

			if (Status != XFSBL_SUCCESS) {
				Status = XFSBL_ERROR_R5_L_TCM_POWER_UP;
				XFsbl_Printf(DEBUG_GENERAL, "XFSBL_ERROR_R5_L_TCM_POWER_UP\r\n");
				goto END;
			}

			/**
			 * Place R5 in lock step mode
			 * Combine TCM's
			 */
			RegValue = XFsbl_In32(RPU_RPU_GLBL_CNTL);
			RegValue |= RPU_RPU_GLBL_CNTL_SLCLAMP_MASK;
			RegValue &= ~(RPU_RPU_GLBL_CNTL_SLSPLIT_MASK);
			RegValue |= RPU_RPU_GLBL_CNTL_TCM_COMB_MASK;
			XFsbl_Out32(RPU_RPU_GLBL_CNTL, RegValue);

			/**
                         * Place R5-0 in HALT state
                         */
                        RegValue = XFsbl_In32(RPU_RPU_0_CFG);
                        RegValue &= ~(RPU_RPU_0_CFG_NCPUHALT_MASK);
                        XFsbl_Out32(RPU_RPU_0_CFG, RegValue);

			/**
                         * Place R5-1 in HALT state
                         */
                        RegValue = XFsbl_In32(RPU_RPU_1_CFG);
                        RegValue &= ~(RPU_RPU_1_CFG_NCPUHALT_MASK);
                        XFsbl_Out32(RPU_RPU_1_CFG, RegValue);

                        /**
                         *  Enable the clock
                         */
                        RegValue = XFsbl_In32(CRL_APB_CPU_R5_CTRL);
                        RegValue |= CRL_APB_CPU_R5_CTRL_CLKACT_MASK;
                        XFsbl_Out32(CRL_APB_CPU_R5_CTRL, RegValue);

                        /**
                         * Provide some delay,
                         * so that clock propogates properly.
                         */
                        (void )usleep(0x50U);

                        /**
                         * Release reset to R5-0,R5-1
                         */
                        RegValue = XFsbl_In32(CRL_APB_RST_LPD_TOP);
                        RegValue &= ~(CRL_APB_RST_LPD_TOP_RPU_R50_RESET_MASK);
                        RegValue &= ~(CRL_APB_RST_LPD_TOP_RPU_R51_RESET_MASK);
                        RegValue &= ~(CRL_APB_RST_LPD_TOP_RPU_AMBA_RESET_MASK);
                        XFsbl_Out32(CRL_APB_RST_LPD_TOP, RegValue);
		} break;

		default:
			/* nothing to do */
			break;
	}

END:
	return Status;
}

void XFsbl_EccInitialize(u32 Address, u32 Length)
{
	u32 Index=0U;

	/* Disable cache to ensure proper ECC initialization */
	Xil_DCacheDisable();
	while (Index<Length)
	{
		XFsbl_Out32(Address+Index, 1U) ;
		Index += 4U;
	}
	Xil_DCacheEnable();

	XFsbl_Printf(DEBUG_INFO,
	  "Address 0x%0lx, Length %0lx, ECC initialized \r\n",
		Address, Length);

	return ;
}


u32 XFsbl_GetLoadAddress(u32 DestinationCpu, PTRSIZE * LoadAddressPtr, u32 Length)
{
	u32 Status = XFSBL_SUCCESS;
	PTRSIZE Address=0U;

	Address = *LoadAddressPtr;

	/**
	 * Update for R50 TCM address
	 */
	if ((DestinationCpu == XIH_PH_ATTRB_DEST_CPU_R5_0) &&
			(Address <= XFSBL_R5_TCM_END_ADDRESS))
	{
		/**
		 * Check if fits to TCM or not
		 */
		if ((Address + Length) > XFSBL_R5_TCM_END_ADDRESS)
		{
			Status = XFSBL_ERROR_LOAD_ADDRESS;
			XFsbl_Printf(DEBUG_GENERAL,
				"XFSBL_ERROR_LOAD_ADDRESS\r\n");
			goto END;
		}

		/**
		 * Update Address to the higher TCM address
		 */
		Address = XFSBL_R50_HIGH_TCM_START_ADDRESS + Address;

	} else
	/**
	 * Update for R5-1 TCM address
	 */
	if ((DestinationCpu == XIH_PH_ATTRB_DEST_CPU_R5_1) &&
			(Address <= XFSBL_R5_TCM_END_ADDRESS))
	{
		/**
		 * Check if fits to TCM or not
		 */
		if ((Address + Length) > XFSBL_R5_TCM_END_ADDRESS)
		{
			Status = XFSBL_ERROR_LOAD_ADDRESS;
			XFsbl_Printf(DEBUG_GENERAL,
				"XFSBL_ERROR_LOAD_ADDRESS\r\n");
			goto END;
		}
		/**
		 * Update Address to the higher TCM address
		 */
		Address = XFSBL_R51_HIGH_TCM_START_ADDRESS + Address;
	} else
	/**
	 * Update for the R5-L TCM address
	 */
	if ((DestinationCpu == XIH_PH_ATTRB_DEST_CPU_R5_L) &&
			(Address <= XFSBL_R5L_TCM_END_ADDRESS))
	{
		/**
		 * Check if fits to TCM or not
		 */
		if ((Address + Length) > XFSBL_R5L_TCM_END_ADDRESS)
		{
			Status = XFSBL_ERROR_LOAD_ADDRESS;
			XFsbl_Printf(DEBUG_GENERAL,
				"XFSBL_ERROR_LOAD_ADDRESS\r\n");
			goto END;
		}
		/**
		 * Update Address to the higher TCM address
		 */
		Address = XFSBL_R50_HIGH_TCM_START_ADDRESS + Address;
	} else
	{
		/**
		 * For MISRA complaince
		 */
	}

	/**
	 * Update the LoadAddress
	 */
	*LoadAddressPtr = Address;

END:
	return Status;
}



/*****************************************************************************/
/**
 * This function calculates the load address based on the destination
 * cpu. For R5 cpu's TCM address is remapped to the higher TCM address
 * so that any cpu can globally access it
 *
 * @param	DestinationCpu is the cpu which partition will run
 *
 * @param	LoadAddress will be updated according to the cpu and address
 *
 * @param	Length of the data to be copied. This is required only to
 *              check for the error case
 *
 * @return	returns the error codes described in xfsbl_error.h on any error
 * 			returns XFSBL_SUCCESS on success
 *****************************************************************************/
static u32 XFsbl_ConfigureMemory(u32 RunningCpu, u32 DestinationCpu,
		u64 Address, u32 Length)
{

	u32 Status = XFSBL_SUCCESS;
	/**
	 * Configure R50 TCM Memory
	 */
	if ((DestinationCpu == XIH_PH_ATTRB_DEST_CPU_R5_0) &&
		(Address >= XFSBL_R50_HIGH_TCM_START_ADDRESS) &&
		(Address < (XFSBL_R50_HIGH_TCM_START_ADDRESS +
				XFSBL_R5_TCM_LENGTH)))
	{

		/**
		 * Power up and release reset to the memory
		 */
		if (RunningCpu != DestinationCpu)
		{
			Status = XFsbl_PowerUpMemory(XFSBL_R5_0_TCM);
			if (Status != XFSBL_SUCCESS) {
				goto END;
			}
		}

		/**
		 * ECC initialize TCM
		 */
		if (IsR50TcmEccInitialized == FALSE)
		{
			XFsbl_EccInitialize(XFSBL_R50_HIGH_TCM_START_ADDRESS,
				XFSBL_R5_TCM_LENGTH);
		}
		IsR50TcmEccInitialized = TRUE;

	} else
	/**
	 * Update for R5-1 TCM address
	 */
	if ((DestinationCpu == XIH_PH_ATTRB_DEST_CPU_R5_1) &&
		(Address >= XFSBL_R51_HIGH_TCM_START_ADDRESS) &&
		(Address < (XFSBL_R51_HIGH_TCM_START_ADDRESS +
				XFSBL_R5_TCM_LENGTH)))
	{
		/**
		 * Power up and release reset to the memory
		 */
		if (RunningCpu != DestinationCpu)
		{
			Status = XFsbl_PowerUpMemory(XFSBL_R5_1_TCM);
			if (Status != XFSBL_SUCCESS) {
				goto END;
			}
		}

		/**
		 * ECC initialize TCM
		 */
		if (IsR51TcmEccInitialized == FALSE)
		{
			XFsbl_EccInitialize(XFSBL_R51_HIGH_TCM_START_ADDRESS,
					XFSBL_R5_TCM_LENGTH);
		}
		IsR51TcmEccInitialized = TRUE;
	} else
	/**
	 * Update for the R5-L TCM address
	 */
	if ((DestinationCpu == XIH_PH_ATTRB_DEST_CPU_R5_L) &&
		(Address >= XFSBL_R50_HIGH_TCM_START_ADDRESS) &&
		(Address < (XFSBL_R50_HIGH_TCM_START_ADDRESS +
				(XFSBL_R5_TCM_LENGTH*2U))))
	{
		/**
		 * Power up and release reset to the memory
		 */
		if (RunningCpu != DestinationCpu)
		{
			Status = XFsbl_PowerUpMemory(XFSBL_R5_L_TCM);
			if (Status != XFSBL_SUCCESS) {
				goto END;
			}

		}

		/**
		 * ECC initialize TCM
		 */
		if (IsR5LTcmEccInitialized == FALSE)
		{
			XFsbl_EccInitialize(XFSBL_R50_HIGH_TCM_START_ADDRESS,
					XFSBL_R5_TCM_LENGTH*2U);
		}
		IsR5LTcmEccInitialized = TRUE;
	} else
	{
		/**
		 * For MISRA complaince
		 */
	}

END:
	return Status;
}


/*****************************************************************************/
/**
 * This function copies the partition to specified destination
 *
 * @param	FsblInstancePtr is pointer to the XFsbl Instance
 *
 * @param	PartitionNum is the partition number in the image to be loaded
 *
 * @return	returns the error codes described in xfsbl_error.h on any error
 * 			returns XFSBL_SUCCESS on success
 *****************************************************************************/
static u32 XFsbl_PartitionCopy(XFsblPs * FsblInstancePtr, u32 PartitionNum)
{
	u32 Status=XFSBL_SUCCESS;
	u32 DestinationCpu=0U;
	u32 CpuNo=0U;
	u32 DestinationDevice=0U;
	u32 ExecState=0U;
	XFsblPs_PartitionHeader * PartitionHeader;
	u32 SrcAddress=0U;
	PTRSIZE LoadAddress=0U;
	u32 Length=0U;
	u32 RunningCpu=0U;

	/**
	 * Assign the partition header to local variable
	 */
	PartitionHeader =
		&FsblInstancePtr->ImageHeader.PartitionHeader[PartitionNum];

	RunningCpu = FsblInstancePtr->ProcessorID;

	/**
	 * Check for XIP image
	 * No need to copy for XIP image
	 */
	DestinationCpu = XFsbl_GetDestinationCpu(PartitionHeader);

	/**
	 * Get the execution state
	 */
	ExecState = XFsbl_GetA53ExecState(PartitionHeader);

	/**
	 * if destination cpu is not present, it means it is for same cpu
	 */
	if (DestinationCpu == XIH_PH_ATTRB_DEST_CPU_NONE)
	{
		DestinationCpu = FsblInstancePtr->ProcessorID;
	}

	if (PartitionHeader->UnEncryptedDataWordLength == 0U)
	{
		/**
		 * Update the Handoff address only for the first application
		 * of that cpu
		 * This is for XIP image. For other partitions it handoff
		 * address is updated after partition validation
		 */
		CpuNo = FsblInstancePtr->HandoffCpuNo;
		if (XFsbl_CheckHandoffCpu(FsblInstancePtr,
				DestinationCpu) == XFSBL_SUCCESS)
		{
			FsblInstancePtr->HandoffValues[CpuNo].CpuSettings =
			        DestinationCpu | ExecState;
			FsblInstancePtr->HandoffValues[CpuNo].HandoffAddress =
				PartitionHeader->DestinationExecutionAddress;
			FsblInstancePtr->HandoffCpuNo += 1U;
		} else {
			/**
			 *
			 * if two partitions has same destination cpu, error can
			 * be triggered here
			 */
		}
		Status = XFSBL_SUCCESS;
		goto END;
	}

	/**
	 * Copy the PL to temporary DDR Address
	 * Copy the PS to Load Address
	 * Copy the PMU firmware to PMU RAM
	 */
	DestinationDevice = XFsbl_GetDestinationDevice(PartitionHeader);
	LoadAddress = PartitionHeader->DestinationLoadAddress;

	if (DestinationDevice == XIH_PH_ATTRB_DEST_DEVICE_PL)
	{
#ifdef XFSBL_BS

		if (LoadAddress == XFSBL_DUMMY_PL_ADDR)
		{
			LoadAddress = XFSBL_DDR_TEMP_ADDRESS;
		}
#else
		XFsbl_Printf(DEBUG_GENERAL,"XFSBL_ERROR_PL_NOT_ENABLED \r\n");
		Status = XFSBL_ERROR_PL_NOT_ENABLED;
		goto END;
#endif
	}

	/**
	 * Get the source(flash offset) address where it needs to copy
	 */
	SrcAddress = FsblInstancePtr->ImageOffsetAddress +
				((PartitionHeader->DataWordOffset) *
					XIH_PARTITION_WORD_LENGTH);

	/**
	 * Length of the partition to be copied
	 */
	Length  = (PartitionHeader->TotalDataWordLength) *
					XIH_PARTITION_WORD_LENGTH;


	/**
	 * When destination device is R5-0/R5-1/R5-L and load address is in TCM
	 * copy to high address of TCM address map
	 * Update the LoadAddress
	 */
	Status = XFsbl_GetLoadAddress(DestinationCpu, &LoadAddress, Length);
	if (XFSBL_SUCCESS != Status)
	{
		goto END;
	}

	/**
	 * Configure the memory
	 */
	XFsbl_ConfigureMemory(RunningCpu, DestinationCpu,
						LoadAddress, Length);

	/**
	 *
	 * Do not copy the IVT if FSBL is running in R5-0/R5-L at 0x0 TCM
	 * Update the SrcAddress, LoadAddress and Len based on the
	 * above condition
	 */
	if (((FsblInstancePtr->ProcessorID ==
			XIH_PH_ATTRB_DEST_CPU_R5_0) ||
		(FsblInstancePtr->ProcessorID ==
				XIH_PH_ATTRB_DEST_CPU_R5_L)) &&
		((LoadAddress > XFSBL_R50_HIGH_TCM_START_ADDRESS) &&
		(LoadAddress <
			XFSBL_R50_HIGH_TCM_START_ADDRESS + XFSBL_IVT_LENGTH)))
	{

		/**
		 * Get the length of the IVT area to be
		 * skipped from Load Address
		 */
		TcmSkipAddress = LoadAddress%XFSBL_IVT_LENGTH;
		TcmSkipLength = XFSBL_IVT_LENGTH - TcmSkipAddress;

		/**
		 * Check if Length is less than SkipLength
		 */
		if (TcmSkipLength > Length)
		{
			TcmSkipLength = Length;
		}

		/**
		 * Copy the Skip length to a local array
		 */
		Status = FsblInstancePtr->DeviceOps.DeviceCopy(SrcAddress,
				(PTRSIZE )TcmVectorArray, TcmSkipLength);
		if (XFSBL_SUCCESS != Status)
		{
			goto END;
		}

		SrcAddress += TcmSkipLength;
		LoadAddress +=  TcmSkipLength;
		Length -= TcmSkipLength;
	}

	if (DestinationDevice == XIH_PH_ATTRB_DEST_DEVICE_PMU)
	{
		/* Enable PMU_0 IPI */
		XFsbl_Out32(IPI_PMU_0_IER, IPI_PMU_0_IER_PMU_0_MASK);

		/* Trigger PMU0 IPI in PMU IPI TRIG Reg */
		XFsbl_Out32(IPI_PMU_0_TRIG, IPI_PMU_0_TRIG_PMU_0_MASK);

		/**
		 * Wait until PMU Microblaze goes to sleep state,
		 * before starting firmware download to PMU RAM
		 */
		while((XFsbl_In32(PMU_GLOBAL_GLOBAL_CNTRL) &
				PMU_GLOBAL_GLOBAL_CNTRL_MB_SLEEP_MASK) !=
						PMU_GLOBAL_GLOBAL_CNTRL_MB_SLEEP_MASK ) {;}
	}

	/**
	 * Copy the partition to PS_DDR/PL_DDR/TCM
	 */
	Status = FsblInstancePtr->DeviceOps.DeviceCopy(SrcAddress,
					LoadAddress, Length);
	if (XFSBL_SUCCESS != Status)
	{
		goto END;
	}

END:
	return Status;
}

/*****************************************************************************/
/**
 * This function validates the partition
 *
 * @param	FsblInstancePtr is pointer to the XFsbl Instance
 *
 * @param	PartitionNum is the partition number in the image to be loaded
 *
 * @return	returns the error codes described in xfsbl_error.h on any error
 * 			returns XFSBL_SUCCESS on success
 *
 *****************************************************************************/
static u32 XFsbl_PartitionValidation(XFsblPs * FsblInstancePtr,
						u32 PartitionNum)
{
	u32 Status=XFSBL_SUCCESS;
	u32 ChecksumType=0U;
	s32 IsEncryptionEnabled=FALSE;
	s32 IsAuthenticationEnabled=FALSE;
	u32 DestinationDevice=0U;
	u32 DestinationCpu=0U;
	u32 ExecState=0U;
	u32 CpuNo=0U;
	XFsblPs_PartitionHeader * PartitionHeader;
#if defined(XFSBL_RSA)
	u32 HashLen=0U;
#endif
#if defined(XFSBL_AES)
	u32 ImageOffset = 0U;
	u32 FsblIv[XIH_BH_IV_LENGTH / 4U];
	u32 UnencryptedLength = 0;
	u32 IvLocation;
#endif
#if defined(XFSBL_RSA) || defined(XFSBL_AES)
	u32 Length=0U;
#endif
#if defined(XFSBL_RSA) || defined(XFSBL_AES) || defined(XFSBL_BS)
	PTRSIZE LoadAddress=0U;
#endif
#if defined(XFSBL_BS)
	u32 BitstreamWordSize = 0;
#endif

	/**
	 * Update the variables
	 */
	PartitionHeader =
	    &FsblInstancePtr->ImageHeader.PartitionHeader[PartitionNum];
	ChecksumType = XFsbl_GetChecksumType(PartitionHeader);

	/**
	 * Check the encryption status
	 */
	if (XFsbl_IsEncrypted(PartitionHeader) ==
			XIH_PH_ATTRB_ENCRYPTION )
	{
		IsEncryptionEnabled = TRUE;

#ifdef XFSBL_AES
		/* Copy the Iv from Flash into local memory */
		IvLocation = ImageOffset + XIH_BH_IV_OFFSET;

		Status = FsblInstancePtr->DeviceOps.DeviceCopy(IvLocation,
				(PTRSIZE) FsblIv, XIH_BH_IV_LENGTH);

		if (Status != XFSBL_SUCCESS) {
			XFsbl_Printf(DEBUG_GENERAL,
					"XFSBL_ERROR_DECRYPTION_IV_COPY_FAIL \r\n");
			Status = XFSBL_ERROR_DECRYPTION_IV_COPY_FAIL;
			goto END;
		}
#else
		XFsbl_Printf(DEBUG_GENERAL,"XFSBL_ERROR_AES_NOT_ENABLED \r\n");
		Status = XFSBL_ERROR_AES_NOT_ENABLED;
		goto END;
#endif
	}

	/**
	 * check the authentication status
	 */
	if (XFsbl_IsRsaSignaturePresent(PartitionHeader) ==
			XIH_PH_ATTRB_RSA_SIGNATURE )
	{
		IsAuthenticationEnabled = TRUE;
	}
	DestinationDevice = XFsbl_GetDestinationDevice(PartitionHeader);
	DestinationCpu = XFsbl_GetDestinationCpu(PartitionHeader);


	/**
         * Get the execution state
         */
        ExecState = XFsbl_GetA53ExecState(PartitionHeader);

	/**
	 * if destination cpu is not present, it means it is for same cpu
	 */
	if (DestinationCpu == XIH_PH_ATTRB_DEST_CPU_NONE)
	{
		DestinationCpu = FsblInstancePtr->ProcessorID;
	}


	/**
	 * Checksum Validation
	 */
	if (ChecksumType == XIH_PH_ATTRB_CHECKSUM_MD5)
	{
		/**
		 * Do the checksum validation
		 */
	}

#if defined(XFSBL_RSA) || defined(XFSBL_AES)
	if ((IsAuthenticationEnabled == TRUE) || (IsEncryptionEnabled == TRUE))
	{
		LoadAddress = PartitionHeader->DestinationLoadAddress;
		Length = PartitionHeader->TotalDataWordLength * 4U;
		Status = XFsbl_GetLoadAddress(DestinationCpu,
				&LoadAddress, Length);
		if (XFSBL_SUCCESS != Status)
		{
			goto END;
		}
	}
#endif

#ifdef XFSBL_BS
	if ((DestinationDevice == XIH_PH_ATTRB_DEST_DEVICE_PL) &&
			(LoadAddress == 0U))
	{
		LoadAddress = XFSBL_DDR_TEMP_ADDRESS;
	}
#endif

	/**
	 * Authentication Check
	 */
	if (IsAuthenticationEnabled == TRUE)
	{
		XFsbl_Printf(DEBUG_INFO,"Authentication Enabled\r\n");
#ifdef XFSBL_RSA
		/**
		 * Get the Sha type to be used from
		 * boot header attributes
		 */
		if ((FsblInstancePtr->BootHdrAttributes &
			XIH_BH_IMAGE_ATTRB_SHA2_MASK) ==
			XIH_BH_IMAGE_ATTRB_SHA2_MASK)
		{
			HashLen = XFSBL_HASH_TYPE_SHA2;
		} else {
			HashLen = XFSBL_HASH_TYPE_SHA3;
		}

		/**
		 * cache disbale can be removed
		 */
		Xil_DCacheDisable();

		/**
		 * Do the authentication validation
		 */
		Status = XFsbl_Authentication(FsblInstancePtr, LoadAddress, Length,
			(LoadAddress + Length) - XFSBL_AUTH_CERT_MIN_SIZE,
			HashLen);

		if (Status != XFSBL_SUCCESS)
                {
                        goto END;
                }
#else
		XFsbl_Printf(DEBUG_GENERAL,"XFSBL_ERROR_RSA_NOT_ENABLED \r\n");
		Status = XFSBL_ERROR_RSA_NOT_ENABLED;
		goto END;
#endif
	}

	/**
	 * Decrypt image through CSU DMA
	 */
	if (IsEncryptionEnabled == TRUE) {
		XFsbl_Printf(DEBUG_INFO, "Decryption Enabled\r\n");
#ifdef XFSBL_AES

		/* AES expects IV in big endian form */
		FsblIv[0] = Xil_Htonl(FsblIv[0]);
		FsblIv[1] = Xil_Htonl(FsblIv[1]);
		FsblIv[2] = Xil_Htonl(FsblIv[2]);

		/* Initialize the Aes Instance so that it's ready to use */
		XSecure_AesInitialize(&SecureAes, &CsuDma,
		 XSECURE_CSU_AES_KEY_SRC_DEV, FsblIv, NULL);

		XFsbl_Printf(DEBUG_INFO, " Aes initialized \r\n");

		UnencryptedLength = PartitionHeader->UnEncryptedDataWordLength * 4U;


		if (DestinationDevice != XIH_PH_ATTRB_DEST_DEVICE_PL) {
			Status = XSecure_AesDecrypt(&SecureAes, (u8 *) LoadAddress,
					(u8 *) LoadAddress, UnencryptedLength);

			if (Status != XFSBL_SUCCESS) {
				Status = XFSBL_ERROR_DECRYPTION_FAIL;
				XFsbl_Printf(DEBUG_GENERAL, "XFSBL_ERROR_DECRYPTION_FAIL\r\n");
				goto END;
			} else {
				XFsbl_Printf(DEBUG_GENERAL, "Decryption Successful\r\n");
			}
		}
#else
		XFsbl_Printf(DEBUG_GENERAL,"XFSBL_ERROR_AES_NOT_ENABLED \r\n");
		Status = XFSBL_ERROR_AES_NOT_ENABLED;
		goto END;
#endif
	}

#ifdef XFSBL_BS
	/**
	 * for PL image use CSU DMA to route to PL
	 */
	if (DestinationDevice == XIH_PH_ATTRB_DEST_DEVICE_PL)
	{
		/**
		 * Fsbl hook before bit stream download
		 */
		Status = XFsbl_HookBeforeBSDownload();
		if (Status != XFSBL_SUCCESS)
		{
			Status = XFSBL_ERROR_HOOK_BEFORE_BITSTREAM_DOWNLOAD;
			XFsbl_Printf(DEBUG_GENERAL,
			 "XFSBL_ERROR_HOOK_BEFORE_BITSTREAM_DOWNLOAD\r\n");
			goto END;
		}

		XFsbl_Printf(DEBUG_GENERAL, "Bitstream download to start now\r\n");

		Status = XFsbl_PcapInit();
		if (Status != XFSBL_SUCCESS) {
			goto END;
		}

		if (IsEncryptionEnabled == TRUE) {
#ifdef XFSBL_AES
			/*
			 * The secure bitstream would be sent through CSU DMA to AES
			 * and the decrypted bitstream is sent directly to PCAP
			 * by configuring SSS appropriately
			 */
			Status = XSecure_AesDecrypt(&SecureAes,
					(u8 *) XFSBL_DESTINATION_PCAP_ADDR,
					(u8 *) LoadAddress, UnencryptedLength);

			if (Status != XFSBL_SUCCESS) {
				Status = XFSBL_ERROR_BITSTREAM_DECRYPTION_FAIL;
				XFsbl_Printf(DEBUG_GENERAL,
						"XFSBL_ERROR_BITSTREAM_DECRYPTION_FAIL\r\n");
				/* Reset PL */
				XFsbl_Out32(CSU_PCAP_PROG, 0x0);
				goto END;
			} else {
				XFsbl_Printf(DEBUG_GENERAL,
						"Bitstream decryption Successful\r\n");
			}
#endif
		}
		else {

			/* Use CSU DMA to load Bit stream to PL */
			BitstreamWordSize = PartitionHeader->UnEncryptedDataWordLength;

			Status = XFsbl_WriteToPcap(BitstreamWordSize, (u8 *) LoadAddress);
			if (Status != XFSBL_SUCCESS) {
				goto END;
			}
		}

		Status = XFsbl_PLWaitForDone();
		if (Status != XFSBL_SUCCESS) {
			goto END;
		}

		/**
		 * Fsbl hook after bit stream download
		 */
		Status = XFsbl_HookAfterBSDownload();
		if (Status != XFSBL_SUCCESS)
		{
			Status = XFSBL_ERROR_HOOK_AFTER_BITSTREAM_DOWNLOAD;
			XFsbl_Printf(DEBUG_GENERAL,
			 "XFSBL_ERROR_HOOK_AFTER_BITSTREAM_DOWNLOAD\r\n");
			goto END;
		}
	}
#endif

	/**
	 * Update the handoff details
	 */
	if ((DestinationDevice != XIH_PH_ATTRB_DEST_DEVICE_PL) &&
			(DestinationDevice != XIH_PH_ATTRB_DEST_DEVICE_PMU))
	{
		CpuNo = FsblInstancePtr->HandoffCpuNo;
		if (XFsbl_CheckHandoffCpu(FsblInstancePtr,
				DestinationCpu) == XFSBL_SUCCESS)
		{
			FsblInstancePtr->HandoffValues[CpuNo].CpuSettings =
					DestinationCpu | ExecState;
			FsblInstancePtr->HandoffValues[CpuNo].HandoffAddress =
					PartitionHeader->DestinationExecutionAddress;
			FsblInstancePtr->HandoffCpuNo += 1U;
		}
	}

END:
	return Status;
}

/*****************************************************************************/
/**
 * This function checks if PMU FW is loaded and gives handoff to PMU Microblaze
 *
 * @param	FsblInstancePtr is pointer to the XFsbl Instance
 *
 * @param	PartitionNum is the partition number of the image
 *
 * @return	None
 *
 *****************************************************************************/
static void XFsbl_CheckPmuFw(XFsblPs * FsblInstancePtr, u32 PartitionNum)
{

	u32 DestinationDev;
	u32 DestinationDevNxt = 0;
	u32 PmuFwLoadDone = FALSE;

	DestinationDev = XFsbl_GetDestinationDevice(
			&FsblInstancePtr->ImageHeader.PartitionHeader[PartitionNum]);

	if (DestinationDev == XIH_PH_ATTRB_DEST_DEVICE_PMU) {
		if ((PartitionNum + 1) <
				(FsblInstancePtr->
						ImageHeader.ImageHeaderTable.NoOfPartitions-1U)) {
			DestinationDevNxt = XFsbl_GetDestinationDevice(
					&FsblInstancePtr->
					ImageHeader.PartitionHeader[PartitionNum + 1]);
			if (DestinationDevNxt != XIH_PH_ATTRB_DEST_DEVICE_PMU) {
				/* there is a partition after this but that is not PMU FW */
				PmuFwLoadDone = TRUE;
			}
		}
		else
		{
			/* the current partition is last PMU FW partition */
			PmuFwLoadDone = TRUE;
		}
	}

	/* If all partitions of PMU FW loaded, handoff it to PMU MicroBlaze */
	if (TRUE == PmuFwLoadDone)
	{
		/* Wakeup the processor */
		XFsbl_Out32(PMU_GLOBAL_GLOBAL_CNTRL,
				XFsbl_In32(PMU_GLOBAL_GLOBAL_CNTRL) | 0x1);

		/* wait until done waking up */
		while((XFsbl_In32(PMU_GLOBAL_GLOBAL_CNTRL) &
				PMU_GLOBAL_GLOBAL_CNTRL_FW_IS_PRESENT_MASK) !=
						PMU_GLOBAL_GLOBAL_CNTRL_FW_IS_PRESENT_MASK ) {;}

	}

}
