/*******************************************************************************
* Copyright (c) 2012-2014, The Microsystems Design Labratory (MDL)
* Department of Computer Science and Engineering, The Pennsylvania State University
* All rights reserved.
* 
* This source code is part of NVMain - A cycle accurate timing, bit accurate
* energy simulator for both volatile (e.g., DRAM) and non-volatile memory
* (e.g., PCRAM). The source code is free and you can redistribute and/or
* modify it by providing that the following conditions are met:
* 
*  1) Redistributions of source code must retain the above copyright notice,
*     this list of conditions and the following disclaimer.
* 
*  2) Redistributions in binary form must reproduce the above copyright notice,
*     this list of conditions and the following disclaimer in the documentation
*     and/or other materials provided with the distribution.
* 
* THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
* ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
* WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
* DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
* FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
* DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
* SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
* CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
* OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
* OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
* 
* Author list: 
*   Matt Poremba    ( Email: mrp5060 at psu dot edu 
*                     Website: http://www.cse.psu.edu/~poremba/ )
*   Tao Zhang       ( Email: tzz106 at cse dot psu dot edu
*                     Website: http://www.cse.psu.edu/~tzz106 )
*******************************************************************************/

#include "src/MemoryController.h"
#include "include/NVMainRequest.h"
#include "src/EventQueue.h"
#include "Interconnect/OffChipBus/OffChipBus.h"
#include "src/Rank.h"
#include "src/Bank.h"
#include "Interconnect/InterconnectFactory.h"

#include <sstream>
#include <cassert>
#include <cstdlib>
#include <csignal>

using namespace NVM;

MemoryController::MemoryController( )
{
    memory = NULL;
    transactionQueues = NULL;

    starvationThreshold = 4;
    subArrayNum = 1;
    starvationCounter = NULL;
    activateQueued = NULL;
    effectiveRow = NULL;
    effectiveMuxedRow = NULL;
    activeSubArray = NULL;

    delayedRefreshCounter = NULL;
    
    curRank = 0;
    curBank = 0;
    nextRefreshRank = 0;
    nextRefreshBank = 0;
}

MemoryController::~MemoryController( )
{

    
    for( ncounter_t i = 0; i < p->RANKS; i++ )
    {
        delete [] bankQueues[i];
        delete [] activateQueued[i];
        delete [] bankNeedRefresh[i];

        for( ncounter_t j = 0; j < p->BANKS; j++ )
        {
            delete [] starvationCounter[i][j];
            delete [] effectiveRow[i][j];
            delete [] effectiveMuxedRow[i][j];
            delete [] activeSubArray[i][j];
        }
        delete [] starvationCounter[i];
        delete [] effectiveRow[i];
        delete [] effectiveMuxedRow[i];
        delete [] activeSubArray[i];
    }

    delete [] bankQueues;
    delete [] starvationCounter;
    delete [] activateQueued;
    delete [] effectiveRow;
    delete [] effectiveMuxedRow;
    delete [] activeSubArray;
    delete [] bankNeedRefresh;
    delete [] rankPowerDown;
    
    if( p->UseRefresh )
    {
        for( ncounter_t i = 0; i < p->RANKS; i++ )
        {
            /* Note: delete a NULL point is permitted in C++ */
            delete [] delayedRefreshCounter[i];
        }
    }

    delete [] delayedRefreshCounter;
    
}

void MemoryController::InitQueues( unsigned int numQueues )
{
    if( transactionQueues != NULL )
        delete [] transactionQueues;

    transactionQueues = new NVMTransactionQueue[ numQueues ];

    for( unsigned int i = 0; i < numQueues; i++ )
        transactionQueues[i].clear( );
}

void MemoryController::Cycle( ncycle_t steps )
{
    GetChild( )->Cycle( steps );
}

bool MemoryController::RequestComplete( NVMainRequest *request )
{
    if( request->type == REFRESH )
        ProcessRefreshPulse( request );
    else if( request->owner == this )
    {
        /* 
         *  Any activate/precharge/etc commands belong to the memory controller
         *  and we are in charge of deleting them!
         */
        delete request;
    }
    else
    {
        return GetParent( )->RequestComplete( request );
    }

    return true;
}

bool MemoryController::IsIssuable( NVMainRequest * /*request*/, FailReason * /*fail*/ )
{
    return true;
}

void MemoryController::SetMappingScheme( )
{
    /* Configure common memory controller parameters. */
    GetDecoder( )->GetTranslationMethod( )->SetAddressMappingScheme( p->AddressMappingScheme );
}

void MemoryController::SetConfig( Config *conf, bool createChildren )
{
    this->config = conf;

    Params *params = new Params( );
    params->SetParams( conf );
    SetParams( params );

    if( createChildren )
    {
        /* When selecting a child, use the bank field from the decoder. */
        AddressTranslator *mcAT = DecoderFactory::CreateDecoderNoWarn( conf->GetString( "Decoder" ) );
        mcAT->SetTranslationMethod( GetParent( )->GetTrampoline( )->GetDecoder( )->GetTranslationMethod( ) );
        mcAT->SetDefaultField( NO_FIELD );
        SetDecoder( mcAT );

        /* Initialize interconnect */
        std::stringstream confString;

        memory = InterconnectFactory::CreateInterconnect( conf->GetString( "INTERCONNECT" ) );

        confString.str( "" );
        confString << StatName( ) << ".channel" << GetID( );
        memory->StatName( confString.str( ) );

        memory->SetParent( this );
        AddChild( memory );

        memory->SetConfig( conf, createChildren );
        memory->RegisterStats( );
        
        SetMappingScheme( );
    }

    /*
     *  The logical bank size is: ROWS * COLS * memory word size (in bytes). 
     *  memory word size (in bytes) is: device width * minimum burst length * data rate / (8 bits/byte) * number of devices
     *  number of devices = bus width / device width
     *  Total channel size is: loglcal bank size * BANKS * RANKS
     */
    std::cout << StatName( ) << " capacity is " << ((p->ROWS * p->COLS * p->DeviceWidth * p->tBURST * p->RATE * (p->BusWidth / p->DeviceWidth) * p->BANKS * p->RANKS) / (8*1024*1024)) << " MB." << std::endl;

    if( conf->KeyExists( "MATHeight" ) )
    {
        subArrayNum = p->ROWS / p->MATHeight;
    }
    else
    {
        subArrayNum = 1;
    }
    
    bankQueues = new std::deque<NVMainRequest *> * [p->RANKS];
    activateQueued = new bool * [p->RANKS];
    starvationCounter = new ncounter_t ** [p->RANKS];
    effectiveRow = new ncounter_t ** [p->RANKS];
    effectiveMuxedRow = new ncounter_t ** [p->RANKS];
    activeSubArray = new ncounter_t ** [p->RANKS];
    rankPowerDown = new bool [p->RANKS];

    for( ncounter_t i = 0; i < p->RANKS; i++ )
    {
        bankQueues[i] = new std::deque<NVMainRequest *> [p->BANKS];
        activateQueued[i] = new bool[p->BANKS];
        activeSubArray[i] = new ncounter_t * [p->BANKS];
        effectiveRow[i] = new ncounter_t * [p->BANKS];
        effectiveMuxedRow[i] = new ncounter_t * [p->BANKS];
        starvationCounter[i] = new ncounter_t * [p->BANKS];

        if( p->UseLowPower )
            rankPowerDown[i] = p->InitPD;
        else
            rankPowerDown[i] = false;

        for( ncounter_t j = 0; j < p->BANKS; j++ )
        {
            activateQueued[i][j] = false;

            starvationCounter[i][j] = new ncounter_t [subArrayNum];
            effectiveRow[i][j] = new ncounter_t [subArrayNum];
            effectiveMuxedRow[i][j] = new ncounter_t [subArrayNum];
            activeSubArray[i][j] = new ncounter_t [subArrayNum];

            for( ncounter_t m = 0; m < subArrayNum; m++ )
            {
                starvationCounter[i][j][m] = 0;
                activeSubArray[i][j][m] = false;
                /* set the initial effective row as invalid */
                effectiveRow[i][j][m] = p->ROWS;
                effectiveMuxedRow[i][j][m] = p->ROWS;
            }
        }
    }

    bankNeedRefresh = new bool * [p->RANKS];
    for( ncounter_t i = 0; i < p->RANKS; i++ )
    {
        bankNeedRefresh[i] = new bool [p->BANKS];
        for( ncounter_t j = 0; j < p->BANKS; j++ )
        {
            bankNeedRefresh[i][j] = false;
        }
    }
        
    delayedRefreshCounter = new ncounter_t * [p->RANKS];

    if( p->UseRefresh )
    {
        /* sanity check */
        assert( p->BanksPerRefresh <= p->BANKS );

        /* 
         * it does not make sense when refresh is needed 
         * but no bank can be refreshed 
         */
        assert( p->BanksPerRefresh != 0 );

        m_refreshBankNum = p->BANKS / p->BanksPerRefresh;
        
        /* first, calculate the tREFI */
        m_tREFI = p->tREFW / (p->ROWS / p->RefreshRows );

        /* then, calculate the time interval between two refreshes */
        ncycle_t m_refreshSlice = m_tREFI / ( p->RANKS * m_refreshBankNum );

        for( ncounter_t i = 0; i < p->RANKS; i++ )
        {
            delayedRefreshCounter[i] = new ncounter_t [m_refreshBankNum];
            
            /* initialize the counter to 0 */
            for( ncounter_t j = 0; j < m_refreshBankNum; j++ )
            {
                delayedRefreshCounter[i][j] = 0;

                ncounter_t refreshBankHead = j * p->BanksPerRefresh;

                /* create first refresh pulse to start the refresh countdown */ 
                NVMainRequest* refreshPulse = MakeRefreshRequest( 
                                                0, 0, refreshBankHead, i, 0 );

                /* stagger the refresh */
                ncycle_t offset = (i * m_refreshBankNum + j ) * m_refreshSlice; 

                /* 
                 * insert refresh pulse, the event queue behaves like a 
                 * refresh countdown timer 
                 */
                GetEventQueue()->InsertEvent( EventResponse, this, refreshPulse, 
                        ( GetEventQueue()->GetCurrentCycle() + m_tREFI + offset ) );
            }
        }
    }

    //this->config->Print();

    SetDebugName( "MemoryController", conf );
}

void MemoryController::RegisterStats( )
{
    AddStat(simulation_cycles);
}

/* 
 * NeedRefresh() has three functions:
 *  1) it returns false when no refresh is used (p->UseRefresh = false) 
 *  2) it returns false if the delayed refresh counter does not
 *  reach the threshold, which provides the flexibility for
 *  fine-granularity refresh 
 *  3) it automatically find the bank group the argument "bank"
 *  specifies and return the result
 */
bool MemoryController::NeedRefresh( const ncounter_t bank, const uint64_t rank )
{
    bool rv = false;

    if( p->UseRefresh )
        if( delayedRefreshCounter[rank][bank/p->BanksPerRefresh] 
                >= p->DelayedRefreshThreshold )
            rv = true;
        
    return rv;
}

/* 
 * Set the refresh flag for a given bank group
 */
void MemoryController::SetRefresh( const ncounter_t bank, const uint64_t rank )
{
    /* align to the head of bank group */
    ncounter_t bankHead = ( bank / p->BanksPerRefresh ) * p->BanksPerRefresh;

    for( ncounter_t i = 0; i < p->BanksPerRefresh; i++ )
        bankNeedRefresh[rank][bankHead + i] = true;
}

/* 
 * Reset the refresh flag for a given bank group
 */
void MemoryController::ResetRefresh( const ncounter_t bank, const uint64_t rank )
{
    /* align to the head of bank group */
    ncounter_t bankHead = ( bank / p->BanksPerRefresh ) * p->BanksPerRefresh;

    for( ncounter_t i = 0; i < p->BanksPerRefresh; i++ )
        bankNeedRefresh[rank][bankHead + i] = false;
}

/* 
 * Increment the delayedRefreshCounter by 1 in a given bank group
 */
void MemoryController::IncrementRefreshCounter( const ncounter_t bank, const uint64_t rank )
{
    /* get the bank group ID */
    ncounter_t bankGroupID = bank / p->BanksPerRefresh;

    delayedRefreshCounter[rank][bankGroupID]++;
}

/* 
 * decrement the delayedRefreshCounter by 1 in a given bank group
 */
void MemoryController::DecrementRefreshCounter( const ncounter_t bank, const uint64_t rank )
{
    /* get the bank group ID */
    ncounter_t bankGroupID = bank / p->BanksPerRefresh;

    delayedRefreshCounter[rank][bankGroupID]--;
}

/* 
 * it simply checks all the banks in the refresh bank group whether their
 * command queues are empty. the result is the union of each check
 */
bool MemoryController::HandleRefresh( )
{
    for( ncounter_t rankIdx = 0; rankIdx < p->RANKS; rankIdx++ )
    {
        ncounter_t i = (nextRefreshRank + rankIdx) % p->RANKS;

        for( ncounter_t bankIdx = 0; bankIdx < m_refreshBankNum; bankIdx++ )
        {
            ncounter_t j = (nextRefreshBank + bankIdx * p->BanksPerRefresh) % p->BANKS;
            FailReason fail;

            if( NeedRefresh( j, i ) && IsRefreshBankQueueEmpty( j , i ) )
            {
                /* create a refresh command that will be sent to ranks */
                NVMainRequest* cmdRefresh = MakeRefreshRequest( 0, 0, j, i, 0 );

                if( GetChild( )->IsIssuable( cmdRefresh, &fail ) == false )
                {
                    for( ncounter_t tmpBank = 0; tmpBank < p->BanksPerRefresh; tmpBank++ ) 
                    {
                        /* Use modulo to allow for an odd number of banks per refresh. */
                        ncounter_t refBank = (tmpBank + j) % p->BANKS;

                        /* Precharge all active banks and active subarrays */
                        if( activateQueued[i][refBank] == true && bankQueues[i][refBank].empty() )
                        {
                            /* issue a PRECHARGE_ALL command to close all subarrays */
                            bankQueues[i][refBank].push_back( 
                                    MakePrechargeAllRequest( 0, 0, refBank, i, 0 ) );

                            /* clear all active subarrays */
                            for( ncounter_t sa = 0; sa < subArrayNum; sa++ )
                            {
                                activeSubArray[i][refBank][sa] = false; 
                                effectiveRow[i][refBank][sa] = p->ROWS;
                                effectiveMuxedRow[i][refBank][sa] = p->ROWS;
                            }
                            activateQueued[i][refBank] = false;
                        }
                    }

                    /* delete the REFRESH command if it can not be issued */
                    delete cmdRefresh;

                    /* check next bank group */
                    continue;
                }

                /* send the refresh command to the rank */
                cmdRefresh->issueCycle = GetEventQueue()->GetCurrentCycle();
                GetChild( )->IssueCommand( cmdRefresh );

                /* decrement the corresponding counter by 1 */
                DecrementRefreshCounter( j, i );

                /* if do not need refresh anymore, reset the refresh flag */
                if( !NeedRefresh( j, i ) )
                    ResetRefresh( j, i );

                /* round-robin */
                nextRefreshBank += p->BanksPerRefresh;
                if( nextRefreshBank >= p->BANKS )
                {
                    nextRefreshBank = 0;
                    nextRefreshRank++;

                    if( nextRefreshRank == p->RANKS )
                        nextRefreshRank = 0;
                }

                /* we should return since one time only one command can be issued */
                return true;  
            }
        }
    }
    return false;
}

/* 
 * it simply increments the corresponding delayed refresh counter 
 * and re-insert the refresh pulse into event queue
 */
void MemoryController::ProcessRefreshPulse( NVMainRequest* refresh )
{
    assert( refresh->type == REFRESH );

    ncounter_t rank, bank;
    refresh->address.GetTranslatedAddress( NULL, NULL, &bank, &rank, NULL, NULL );

    IncrementRefreshCounter( bank, rank );

    if( NeedRefresh( bank, rank ) )
        SetRefresh( bank, rank ); 

    GetEventQueue()->InsertEvent( EventResponse, this, refresh, 
            ( GetEventQueue()->GetCurrentCycle() + m_tREFI ) );
}

/* 
 * it simply checks all banks in the refresh bank group whether their
 * command queues are empty. the result is the union of each check
 */
bool MemoryController::IsRefreshBankQueueEmpty( const ncounter_t bank, const uint64_t rank )
{
    /* align to the head of bank group */
    ncounter_t bankHead = ( bank / p->BanksPerRefresh ) * p->BanksPerRefresh;

    for( ncounter_t i = 0; i < p->BanksPerRefresh; i++ )
        if( !bankQueues[rank][bankHead + i].empty() )
            return false;

    return true;
}

void MemoryController::PowerDown( const ncounter_t& rankId )
{
    OpType pdOp;
    if( p->PowerDownMode == "SLOWEXIT" )
        pdOp = POWERDOWN_PDPS;
    else if( p->PowerDownMode == "FASTEXIT" )
        pdOp = POWERDOWN_PDPF;
    else
        std::cerr << "NVMain Error: Undefined low power mode" << std::endl;

    /* if some banks are active, active powerdown is applied */
    if( ((Interconnect*)GetChild()->GetTrampoline())->IsRankIdle( rankId ) == false )
        pdOp = POWERDOWN_PDA;

    if( ((Interconnect*)GetChild()->GetTrampoline())->CanPowerDown( pdOp, rankId ) 
        && RankQueueEmpty( rankId ) )
    {
        ((Interconnect*)GetChild()->GetTrampoline())->PowerDown( pdOp, rankId );
        rankPowerDown[rankId] = true;
    }
}

void MemoryController::PowerUp( const ncounter_t& rankId )
{
    /* if some banks are active, active powerdown is applied */
    if( RankQueueEmpty( rankId ) == false 
        && ((Interconnect*)GetChild()->GetTrampoline())->CanPowerUp( rankId ) )
    {
        ((Interconnect*)GetChild()->GetTrampoline())->PowerUp( rankId );
        rankPowerDown[rankId] = false;
    }
}

void MemoryController::HandleLowPower( )
{
    for( ncounter_t rankId = 0; rankId < p->RANKS; rankId++ )
    {
        bool needRefresh = false;
        for( ncounter_t bankId = 0; bankId < m_refreshBankNum; bankId++ )
        {
            ncounter_t bankGroupHead = bankId * p->BanksPerRefresh;

            if( NeedRefresh( bankGroupHead, rankId ) )
            {
                needRefresh = true;
                break;
            }
        }

        /* if some of the banks in this rank need refresh */
        if( needRefresh )
        {
            /* if the rank is powered down, power it up */
            if( rankPowerDown[rankId] && ((Interconnect*)GetChild()->GetTrampoline())->CanPowerUp( rankId ) )
            {
                ((Interconnect*)GetChild()->GetTrampoline())->PowerUp( rankId );
                rankPowerDown[rankId] = false;
            }
        }
        /* else, check whether the rank can be powered down or up */
        else
        {
            if( rankPowerDown[rankId] )
                PowerUp( rankId );
            else
                PowerDown( rankId );
        }
    }
}

Config *MemoryController::GetConfig( )
{
    return (this->config);
}

void MemoryController::SetID( unsigned int id )
{
    this->id = id;
}

unsigned int MemoryController::GetID( )
{
    return this->id;
}

NVMainRequest *MemoryController::MakeActivateRequest( NVMainRequest *triggerRequest )
{
    NVMainRequest *activateRequest = new NVMainRequest( );

    activateRequest->type = ACTIVATE;
    activateRequest->issueCycle = GetEventQueue()->GetCurrentCycle();
    activateRequest->address = triggerRequest->address;
    activateRequest->owner = this;

    return activateRequest;
}

NVMainRequest *MemoryController::MakeActivateRequest( const ncounter_t row,
                                                      const ncounter_t col,
                                                      const ncounter_t bank,
                                                      const ncounter_t rank,
                                                      const ncounter_t subarray )
{
    NVMainRequest *activateRequest = new NVMainRequest( );

    activateRequest->type = ACTIVATE;
    ncounter_t actAddr = GetDecoder( )->ReverseTranslate( row, col, bank, rank, 0, subarray );
    activateRequest->address.SetPhysicalAddress( actAddr );
    activateRequest->address.SetTranslatedAddress( row, col, bank, rank, 0, subarray );
    activateRequest->issueCycle = GetEventQueue()->GetCurrentCycle();
    activateRequest->owner = this;

    return activateRequest;
}

NVMainRequest *MemoryController::MakePrechargeRequest( NVMainRequest *triggerRequest )
{
    NVMainRequest *prechargeRequest = new NVMainRequest( );

    prechargeRequest->type = PRECHARGE;
    prechargeRequest->issueCycle = GetEventQueue()->GetCurrentCycle();
    prechargeRequest->address = triggerRequest->address;
    prechargeRequest->owner = this;

    return prechargeRequest;
}

NVMainRequest *MemoryController::MakePrechargeRequest( const ncounter_t row,
                                                       const ncounter_t col,
                                                       const ncounter_t bank,
                                                       const ncounter_t rank,
                                                       const ncounter_t subarray )
{
    NVMainRequest *prechargeRequest = new NVMainRequest( );

    prechargeRequest->type = PRECHARGE;
    ncounter_t preAddr = GetDecoder( )->ReverseTranslate( row, col, bank, rank, 0, subarray );
    prechargeRequest->address.SetPhysicalAddress( preAddr );
    prechargeRequest->address.SetTranslatedAddress( row, col, bank, rank, 0, subarray );
    prechargeRequest->issueCycle = GetEventQueue()->GetCurrentCycle();
    prechargeRequest->owner = this;

    return prechargeRequest;
}

NVMainRequest *MemoryController::MakePrechargeAllRequest( NVMainRequest *triggerRequest )
{
    NVMainRequest *prechargeAllRequest = new NVMainRequest( );

    prechargeAllRequest->type = PRECHARGE_ALL;
    prechargeAllRequest->issueCycle = GetEventQueue()->GetCurrentCycle();
    prechargeAllRequest->address = triggerRequest->address;
    prechargeAllRequest->owner = this;

    return prechargeAllRequest;
}

NVMainRequest *MemoryController::MakePrechargeAllRequest( const ncounter_t row,
                                                          const ncounter_t col,
                                                          const ncounter_t bank,
                                                          const ncounter_t rank,
                                                          const ncounter_t subarray )
{
    NVMainRequest *prechargeAllRequest = new NVMainRequest( );

    prechargeAllRequest->type = PRECHARGE_ALL;
    ncounter_t preAddr = GetDecoder( )->ReverseTranslate( row, col, bank, rank, 0, subarray );
    prechargeAllRequest->address.SetPhysicalAddress( preAddr );
    prechargeAllRequest->address.SetTranslatedAddress( row, col, bank, rank, 0, subarray );
    prechargeAllRequest->issueCycle = GetEventQueue()->GetCurrentCycle();
    prechargeAllRequest->owner = this;

    return prechargeAllRequest;
}

NVMainRequest *MemoryController::MakeImplicitPrechargeRequest( NVMainRequest *triggerRequest )
{
    if( triggerRequest->type == READ )
        triggerRequest->type = READ_PRECHARGE;
    else if( triggerRequest->type == WRITE )
        triggerRequest->type = WRITE_PRECHARGE;

    triggerRequest->issueCycle = GetEventQueue()->GetCurrentCycle();

    return triggerRequest;
}

NVMainRequest *MemoryController::MakeRefreshRequest( const ncounter_t row,
                                                     const ncounter_t col,
                                                     const ncounter_t bank,
                                                     const ncounter_t rank,
                                                     const ncounter_t subarray )
{
    NVMainRequest *refreshRequest = new NVMainRequest( );

    refreshRequest->type = REFRESH;
    ncounter_t preAddr = GetDecoder( )->ReverseTranslate( row, col, bank, rank, 0, subarray );
    refreshRequest->address.SetPhysicalAddress( preAddr );
    refreshRequest->address.SetTranslatedAddress( row, col, bank, rank, 0, subarray );
    refreshRequest->issueCycle = GetEventQueue()->GetCurrentCycle();
    refreshRequest->owner = this;

    return refreshRequest;
}

bool MemoryController::IsLastRequest( std::list<NVMainRequest *>& transactionQueue,
                                      NVMainRequest *request )
{
    bool rv = true;
    
    if( p->ClosePage == 0 )
        rv = false;
    else if( p->ClosePage == 1 )
    {
        ncounter_t mRank, mBank, mRow, mSubArray;
        request->address.GetTranslatedAddress( &mRow, NULL, &mBank, &mRank, NULL, &mSubArray );
        std::list<NVMainRequest *>::iterator it;

        for( it = transactionQueue.begin(); it != transactionQueue.end(); it++ )
        {
            ncounter_t rank, bank, row, subarray;

            (*it)->address.GetTranslatedAddress( &row, NULL, &bank, &rank, NULL, &subarray );

            /* if a request that has row buffer hit is found, return false */ 
            if( rank == mRank && bank == mBank && row == mRow && subarray == mSubArray )
            {
                rv = false;
                break;
            }
        }
    }

    return rv;
}

bool MemoryController::FindStarvedRequest( std::list<NVMainRequest *>& transactionQueue, 
                                           NVMainRequest **starvedRequest )
{
    DummyPredicate pred;

    return FindStarvedRequest( transactionQueue, starvedRequest, pred );
}

bool MemoryController::FindStarvedRequest( std::list<NVMainRequest *>& transactionQueue, 
                                           NVMainRequest **starvedRequest, 
                                           SchedulingPredicate& pred )
{
    bool rv = false;
    std::list<NVMainRequest *>::iterator it;

    for( it = transactionQueue.begin(); it != transactionQueue.end(); it++ )
    {
        ncounter_t rank, bank, row, subarray, col;

        (*it)->address.GetTranslatedAddress( &row, &col, &bank, &rank, NULL, &subarray );
        
        /* By design, mux level can only be a subset of the selected columns. */
        ncounter_t muxLevel = static_cast<ncounter_t>(col / p->RBSize);

        if( activateQueued[rank][bank] 
            && ( !activeSubArray[rank][bank][subarray]          /* The subarray is inactive */
                || effectiveRow[rank][bank][subarray] != row    /* Row buffer miss */
                || effectiveMuxedRow[rank][bank][subarray] != muxLevel )  /* Subset of row buffer is not at the sense amps */
            && !bankNeedRefresh[rank][bank]                     /* The bank is not waiting for a refresh */
            && starvationCounter[rank][bank][subarray] 
                >= starvationThreshold                          /* This subarray has reached starvation threshold */
            && bankQueues[rank][bank].empty()                   /* The request queue is empty */
            && pred( (*it) ) )                                  /* User-defined predicate is true */
        {
            *starvedRequest = (*it);
            transactionQueue.erase( it );

            /* Different row buffer management policy has different behavior */ 

            /* 
             * if Relaxed Close-Page row buffer management policy is applied,
             * we check whether there is another request has row buffer hit.
             * if not, this request is the last request and we can close the
             * row.
             */
            if(  IsLastRequest( transactionQueue, (*starvedRequest) ) )
                (*starvedRequest)->flags |= NVMainRequest::FLAG_LAST_REQUEST;

            rv = true;
            break;
        }
    }

    return rv;
}

bool MemoryController::FindWriteStalledRead( std::list<NVMainRequest *>& transactionQueue,
                                             NVMainRequest **hitRequest )
{
    DummyPredicate pred;

    return FindWriteStalledRead( transactionQueue, hitRequest, pred );
}

bool MemoryController::FindWriteStalledRead( std::list<NVMainRequest *>& transactionQueue, 
                                             NVMainRequest **hitRequest, SchedulingPredicate& pred )
{
    bool rv = false;
    std::list<NVMainRequest *>::iterator it;

    if( !p->WritePausing )
        return false;

    for( it = transactionQueue.begin(); it != transactionQueue.end(); it++ )
    {
        if( (*it)->type != READ )
            continue;

        ncounter_t rank, bank, row, subarray, col;

        (*it)->address.GetTranslatedAddress( &row, &col, &bank, &rank, NULL, &subarray );

        /* By design, mux level can only be a subset of the selected columns. */
        ncounter_t muxLevel = static_cast<ncounter_t>(col / p->RBSize);

        /* Find the requests's SubArray destination. */
        SubArray *writingArray = FindChild( (*it), SubArray );

        //if( writingArray->isWriting )
        //{
        //    std::cout << "Subarray is writing!" << std::endl;
        //}


        if( activateQueued[rank][bank]                    /* The bank is active */ 
            && activeSubArray[rank][bank][subarray]       /* The subarray is open */
            && effectiveRow[rank][bank][subarray] == row  /* The effective row is the row of this request */ 
            && effectiveMuxedRow[rank][bank][subarray] == muxLevel  /* Subset of row buffer is currently at the sense amps */
            && !bankNeedRefresh[rank][bank]               /* The bank is not waiting for a refresh */
            && writingArray->isWriting                    /* There needs to be a write to cancel. */
            && GetChild( )->IsIssuable( (*it ) )          /* Make sure we can actually send this right away, otherwise we may skip starvation/other checks in MC. */
            && pred( (*it) ) )                            /* User-defined predicate is true */
        {
            *hitRequest = (*it);
            transactionQueue.erase( it );

            /* Different row buffer management policy has different behavior */ 

            /* 
             * if Relaxed Close-Page row buffer management policy is applied,
             * we check whether there is another request has row buffer hit.
             * if not, this request is the last request and we can close the
             * row.
             */
            if( IsLastRequest( transactionQueue, (*hitRequest) ) )
                (*hitRequest)->flags |= NVMainRequest::FLAG_LAST_REQUEST;

            rv = true;

            break;
        }
    }

    return rv;
}

bool MemoryController::FindRowBufferHit( std::list<NVMainRequest *>& transactionQueue, 
                                         NVMainRequest **hitRequest )
{
    DummyPredicate pred;

    return FindRowBufferHit( transactionQueue, hitRequest, pred );
}

bool MemoryController::FindRowBufferHit( std::list<NVMainRequest *>& transactionQueue, 
                                         NVMainRequest **hitRequest, SchedulingPredicate& pred )
{
    bool rv = false;
    std::list<NVMainRequest *>::iterator it;

    for( it = transactionQueue.begin(); it != transactionQueue.end(); it++ )
    {
        ncounter_t rank, bank, row, subarray, col;

        (*it)->address.GetTranslatedAddress( &row, &col, &bank, &rank, NULL, &subarray );

        /* By design, mux level can only be a subset of the selected columns. */
        ncounter_t muxLevel = static_cast<ncounter_t>(col / p->RBSize);

        if( activateQueued[rank][bank]                    /* The bank is active */ 
            && activeSubArray[rank][bank][subarray]       /* The subarray is open */
            && effectiveRow[rank][bank][subarray] == row  /* The effective row is the row of this request */ 
            && effectiveMuxedRow[rank][bank][subarray] == muxLevel  /* Subset of row buffer is currently at the sense amps */
            && !bankNeedRefresh[rank][bank]               /* The bank is not waiting for a refresh */
            && bankQueues[rank][bank].empty( )            /* The request queue is empty */
            && pred( (*it) ) )                            /* User-defined predicate is true */
        {
            *hitRequest = (*it);
            transactionQueue.erase( it );

            /* Different row buffer management policy has different behavior */ 

            /* 
             * if Relaxed Close-Page row buffer management policy is applied,
             * we check whether there is another request has row buffer hit.
             * if not, this request is the last request and we can close the
             * row.
             */
            if( IsLastRequest( transactionQueue, (*hitRequest) ) )
                (*hitRequest)->flags |= NVMainRequest::FLAG_LAST_REQUEST;

            rv = true;

            break;
        }
    }

    return rv;
}

bool MemoryController::FindOldestReadyRequest( std::list<NVMainRequest *>& transactionQueue, 
                                               NVMainRequest **oldestRequest )
{
    DummyPredicate pred;

    return FindOldestReadyRequest( transactionQueue, oldestRequest, pred );
}

bool MemoryController::FindOldestReadyRequest( std::list<NVMainRequest *>& transactionQueue, 
                                               NVMainRequest **oldestRequest, 
                                               SchedulingPredicate& pred )
{
    bool rv = false;
    std::list<NVMainRequest *>::iterator it;

    for( it = transactionQueue.begin(); it != transactionQueue.end(); it++ )
    {
        ncounter_t rank, bank;

        (*it)->address.GetTranslatedAddress( NULL, NULL, &bank, &rank, NULL, NULL );

        if( activateQueued[rank][bank]         /* The bank is active */ 
            && !bankNeedRefresh[rank][bank]    /* The bank is not waiting for a refresh */
            && bankQueues[rank][bank].empty()  /* The request queue is empty */
            && pred( (*it) ) )                 /* User-defined predicate is true. */
        {
            *oldestRequest = (*it);
            transactionQueue.erase( it );
            
            /* Different row buffer management policy has different behavior */ 

            /* 
             * if Relaxed Close-Page row buffer management policy is applied,
             * we check whether there is another request has row buffer hit.
             * if not, this request is the last request and we can close the
             * row.
             */
            if( IsLastRequest( transactionQueue, (*oldestRequest) ) )
                (*oldestRequest)->flags |= NVMainRequest::FLAG_LAST_REQUEST;

            rv = true;
            break;
        }
    }

    return rv;
}

bool MemoryController::FindClosedBankRequest( std::list<NVMainRequest *>& transactionQueue, 
                                              NVMainRequest **closedRequest )
{
    DummyPredicate pred;

    return FindClosedBankRequest( transactionQueue, closedRequest, pred );
}

bool MemoryController::FindClosedBankRequest( std::list<NVMainRequest *>& transactionQueue, 
                                              NVMainRequest **closedRequest, 
                                              SchedulingPredicate& pred )
{
    bool rv = false;
    std::list<NVMainRequest *>::iterator it;

    for( it = transactionQueue.begin(); it != transactionQueue.end(); it++ )
    {
        ncounter_t rank, bank;

        (*it)->address.GetTranslatedAddress( NULL, NULL, &bank, &rank, NULL, NULL );

        if( !activateQueued[rank][bank]         /* This bank is inactive */
            && !bankNeedRefresh[rank][bank]     /* The bank is not waiting for a refresh */
            && bankQueues[rank][bank].empty()   /* The request queue is empty */
            && pred( (*it) ) )                  /* User defined predicate is true. */
        {
            *closedRequest = (*it);
            transactionQueue.erase( it );
            
            /* Different row buffer management policy has different behavior */ 

            /* 
             * if Relaxed Close-Page row buffer management policy is applied,
             * we check whether there is another request has row buffer hit.
             * if not, this request is the last request and we can close the
             * row.
             */
            if( IsLastRequest( transactionQueue, (*closedRequest) ) )
                (*closedRequest)->flags |= NVMainRequest::FLAG_LAST_REQUEST;

            rv = true;
            break;
        }
    }

    return rv;
}

/*
 *  Slightly modify the scheduling functions form MemoryController.cpp to return a list instead
 *  of just a single request
 */
bool MemoryController::FindStarvedRequests( std::list<NVMainRequest *>& transactionQueue, 
                                            std::vector<NVMainRequest *>& starvedRequests )
{
    DummyPredicate pred;

    return FindStarvedRequests( transactionQueue, starvedRequests );
}

bool MemoryController::FindStarvedRequests( std::list<NVMainRequest *>& transactionQueue, 
                                            std::vector<NVMainRequest *>& starvedRequests, 
                                            SchedulingPredicate& pred )
{
    bool rv = false;
    std::list<NVMainRequest *>::iterator it;

    for( it = transactionQueue.begin(); it != transactionQueue.end(); it++ )
    {
        ncounter_t rank, bank, row, subarray, col;

        (*it)->address.GetTranslatedAddress( &row, &col, &bank, &rank, NULL, &subarray );

        ncounter_t muxLevel = static_cast<ncounter_t>(col / p->RBSize);

        if( activateQueued[rank][bank]                         /* The bank is active */ 
            && ( !activeSubArray[rank][bank][subarray]         /* The subarray is closed */ 
                || effectiveRow[rank][bank][subarray] != row   /* The effective row is not the row of this request */
                || effectiveMuxedRow[rank][bank][subarray] != muxLevel )  /* Subset of row buffer is not at the sense amps. */
            && !bankNeedRefresh[rank][bank]                    /* The bank is not waiting for a refresh */
            && starvationCounter[rank][bank][subarray] 
                >= starvationThreshold                         /* The subarray has reached starvation threshold */
            && bankQueues[rank][bank].empty()                  /* The request queue is empty */
            && pred( (*it) ) )                                 /* User-defined predicate is true. */
        {
            starvedRequests.push_back( (*it) );
            transactionQueue.erase( it );

            rv = true;
        }
    }

    return rv;
}

bool MemoryController::FindRowBufferHits( std::list<NVMainRequest *>& transactionQueue, 
                                          std::vector<NVMainRequest *>& hitRequests )
{
    DummyPredicate pred;

    return FindRowBufferHits( transactionQueue, hitRequests, pred );
}

bool MemoryController::FindRowBufferHits( std::list<NVMainRequest *>& transactionQueue, 
                                          std::vector<NVMainRequest* >& hitRequests, 
                                          SchedulingPredicate& pred )
{
    bool rv = false;
    std::list<NVMainRequest *>::iterator it;

    for( it = transactionQueue.begin(); it != transactionQueue.end(); it++ )
    {
        ncounter_t rank, bank, row, subarray, col;

        (*it)->address.GetTranslatedAddress( &row, &col, &bank, &rank, NULL, &subarray );

        ncounter_t muxLevel= static_cast<ncounter_t>(col / p->RBSize);

        if( activateQueued[rank][bank]                      /* The bank is active */ 
            && activeSubArray[rank][bank][subarray]         /* The subarray is open */
            && effectiveRow[rank][bank][subarray] == row    /* The effective row is the row of this request. */
            && effectiveMuxedRow[rank][bank][subarray] == muxLevel  /* Subset of row buffer is at the sense amps. */
            && !bankNeedRefresh[rank][bank]                 /* The bank is not wating for a refresh*/
            && bankQueues[rank][bank].empty( )              /* The request queue is empty */
            && pred( (*it) ) )                              /* User-defined predicate is true. */
        {
            hitRequests.push_back( (*it) );
            transactionQueue.erase( it );

            rv = true;
        }
    }

    return rv;
}

bool MemoryController::FindOldestReadyRequests( std::list<NVMainRequest *>& transactionQueue, 
                                                std::vector<NVMainRequest *> &oldestRequests )
{
    DummyPredicate pred;

    return FindOldestReadyRequests( transactionQueue, oldestRequests, pred );
}

bool MemoryController::FindOldestReadyRequests( std::list<NVMainRequest *>& transactionQueue, 
                                                std::vector<NVMainRequest *>& oldestRequests, 
                                                SchedulingPredicate& pred )
{
    bool rv = false;
    std::list<NVMainRequest *>::iterator it;

    for( it = transactionQueue.begin(); it != transactionQueue.end(); it++ )
    {
        ncounter_t rank, bank;

        (*it)->address.GetTranslatedAddress( NULL, NULL, &bank, &rank, NULL, NULL );

        if( activateQueued[rank][bank]         /* The bank is active */
            && !bankNeedRefresh[rank][bank]    /* The bank is not waiting for a refresh */
            && bankQueues[rank][bank].empty()  /* The request queue is empty */
            && pred( (*it) ) )                 /* User-defined predicate is true */
        {
            oldestRequests.push_back( (*it) );
            transactionQueue.erase( it );
            
            rv = true;
        }
    }

    return rv;
}

bool MemoryController::FindClosedBankRequests( std::list<NVMainRequest *>& transactionQueue, 
                                               std::vector<NVMainRequest *> &closedRequests )
{
    DummyPredicate pred;

    return FindClosedBankRequests( transactionQueue, closedRequests, pred );
}

bool MemoryController::FindClosedBankRequests( std::list<NVMainRequest *>& transactionQueue, 
        std::vector<NVMainRequest *> &closedRequests, SchedulingPredicate& pred )
{
    bool rv = false;
    std::list<NVMainRequest *>::iterator it;

    for( it = transactionQueue.begin(); it != transactionQueue.end(); it++ )
    {
        ncounter_t rank, bank;

        (*it)->address.GetTranslatedAddress( NULL, NULL, &bank, &rank, NULL, NULL );

        if( !activateQueued[rank][bank]        /* This bank is closed, anyone can issue. */
            && !bankNeedRefresh[rank][bank]    /* request is blocked when refresh is needed */
            && bankQueues[rank][bank].empty()  /* No requests are currently issued to this bank (Ready). */
            && pred( (*it) ) )       /* User defined predicate is true. */
        {
            closedRequests.push_back( (*it ) );
            transactionQueue.erase( it );
            
            rv = true;
        }
    }

    return rv;
}

bool MemoryController::DummyPredicate::operator() ( NVMainRequest* /*request*/ )
{
    return true;
}


bool MemoryController::IssueMemoryCommands( NVMainRequest *req )
{
    bool rv = false;
    ncounter_t rank, bank, row, subarray, col;

    req->address.GetTranslatedAddress( &row, &col, &bank, &rank, NULL, &subarray );

    ncounter_t muxLevel = static_cast<ncounter_t>(col / p->RBSize);

    /*
     *  This function assumes the memory controller uses any predicates when
     *  scheduling. They will not be re-checked here.
     */

    if( !activateQueued[rank][bank] && bankQueues[rank][bank].empty() )
    {
        /* Any activate will request the starvation counter */
        activateQueued[rank][bank] = true;
        activeSubArray[rank][bank][subarray] = true;
        effectiveRow[rank][bank][subarray] = row;
        effectiveMuxedRow[rank][bank][subarray] = muxLevel;
        starvationCounter[rank][bank][subarray] = 0;

        req->issueCycle = GetEventQueue()->GetCurrentCycle();

        bankQueues[rank][bank].push_back( MakeActivateRequest( req ) );

        /* Different row buffer management policy has different behavior */ 
        /*
         * There are two possibilities that the request is the last request:
         * 1) ClosePage == 1 and there is no other request having row
         * buffer hit
         * or 2) ClosePage == 2, the request is always the last request
         */
        if( req->flags & NVMainRequest::FLAG_LAST_REQUEST )
        {
            bankQueues[rank][bank].push_back( MakeImplicitPrechargeRequest( req ) );
            activeSubArray[rank][bank][subarray] = false;
            effectiveRow[rank][bank][subarray] = p->ROWS;
            effectiveMuxedRow[rank][bank][subarray] = p->ROWS;
            activateQueued[rank][bank] = false;
        }
        else
            bankQueues[rank][bank].push_back( req );

        rv = true;
    }
    else if( activateQueued[rank][bank] 
            && ( !activeSubArray[rank][bank][subarray] 
                || effectiveRow[rank][bank][subarray] != row 
                || effectiveMuxedRow[rank][bank][subarray] != muxLevel )
            && bankQueues[rank][bank].empty() )
    {
        /* Any activate will request the starvation counter */
        starvationCounter[rank][bank][subarray] = 0;
        activateQueued[rank][bank] = true;

        req->issueCycle = GetEventQueue()->GetCurrentCycle();

        if( activeSubArray[rank][bank][subarray] )
        {
            bankQueues[rank][bank].push_back( 
                    MakePrechargeRequest( effectiveRow[rank][bank][subarray], 0, bank, rank, subarray ) );
        }

        bankQueues[rank][bank].push_back( MakeActivateRequest( req ) );
        bankQueues[rank][bank].push_back( req );
        activeSubArray[rank][bank][subarray] = true;
        effectiveRow[rank][bank][subarray] = row;
        effectiveMuxedRow[rank][bank][subarray] = muxLevel;

        rv = true;
    }
    else if( activateQueued[rank][bank] 
            && activeSubArray[rank][bank][subarray]
            && effectiveRow[rank][bank][subarray] == row 
            && effectiveMuxedRow[rank][bank][subarray] == muxLevel )
    {
        starvationCounter[rank][bank][subarray]++;

        req->issueCycle = GetEventQueue()->GetCurrentCycle();

        /* Different row buffer management policy has different behavior */ 
        /*
         * There are two possibilities that the request is the last request:
         * 1) ClosePage == 1 and there is no other request having row
         * buffer hit
         * or 2) ClosePage == 2, the request is always the last request
         */
        if( req->flags & NVMainRequest::FLAG_LAST_REQUEST )
        {
            /* if Restricted Close-Page is applied, we should never be here */
            assert( p->ClosePage != 2 );

            bankQueues[rank][bank].push_back( MakeImplicitPrechargeRequest( req ) );
            activeSubArray[rank][bank][subarray] = false;
            effectiveRow[rank][bank][subarray] = p->ROWS;
            effectiveMuxedRow[rank][bank][subarray] = p->ROWS;

            bool idle = true;
            for( ncounter_t i = 0; i < subArrayNum; i++ )
            {
                if( activeSubArray[rank][bank][i] == true )
                {
                    idle = false;
                    break;
                }
            }

            if( idle )
                activateQueued[rank][bank] = false;
        }
        else
            bankQueues[rank][bank].push_back( req );

        rv = true;
    }
    else
    {
        rv = false;
    }

    return rv;
}

void MemoryController::CycleCommandQueues( )
{
    if( p->UseLowPower )
        HandleLowPower( );

    /* First of all, see whether we can issue a necessary refresh */
    if( p->UseRefresh ) 
    {
        if( HandleRefresh( ) )
            return;
    }

    for( ncounter_t rankIdx = 0; rankIdx < p->RANKS; rankIdx++ )
    {
        ncounter_t i = (curRank + rankIdx)%p->RANKS;

        for( ncounter_t bankIdx = 0; bankIdx < p->BANKS; bankIdx++ )
        {
            ncounter_t j = (curBank + bankIdx)%p->BANKS;
            FailReason fail;

            if( !bankQueues[i][j].empty( )
                && GetChild( )->IsIssuable( bankQueues[i][j].at( 0 ), &fail ) )
            {
                *debugStream << "MemoryContoller: Issued request type " << bankQueues[i][j].at(0)->type
                             << " for address Ox" << std::hex << bankQueues[i][j].at(0)->address.GetPhysicalAddress()
                             << std::dec << std::endl;

                GetChild( )->IssueCommand( bankQueues[i][j].at( 0 ) );

                bankQueues[i][j].erase( bankQueues[i][j].begin( ) );

                MoveRankBank();

                /* we should return since one time only one command can be issued */
                return;
            }
            else if( !bankQueues[i][j].empty( ) )
            {
                NVMainRequest *queueHead = bankQueues[i][j].at( 0 );

                if( ( GetEventQueue()->GetCurrentCycle() - queueHead->issueCycle ) > p->DeadlockTimer )
                {
                    ncounter_t row, col, bank, rank, channel, subarray;
                    queueHead->address.GetTranslatedAddress( &row, &col, &bank, &rank, &channel, &subarray );
                    std::cout << "NVMain Warning: Operation could not be sent to memory after a very long time: "
                              << std::endl; 
                    std::cout << "         Address: 0x" << std::hex 
                              << queueHead->address.GetPhysicalAddress( )
                              << std::dec << " @ Bank " << bank << ", Rank " << rank << ", Channel " << channel
                              << " Subarray " << subarray << " Row " << row << " Column " << col
                              << ". Queued time: " << queueHead->arrivalCycle
                              << ". Current time: " << GetEventQueue()->GetCurrentCycle() << ". Type: " 
                              << queueHead->type << std::endl;

                    // Give the opportunity to attach a debugger here.
#ifndef NDEBUG
                    raise( SIGSTOP );
#endif
                    GetStats( )->PrintAll( std::cerr );
                    exit(1);
                }
            }
        }
    }
}

/*
 * RankQueueEmpty() check all command queues in the given rank to see whether
 * they are empty, return true if all queues are empty
 */
bool MemoryController::RankQueueEmpty( const ncounter_t& rankId )
{
    bool rv = true;

    for( ncounter_t i = 0; i < p->BANKS; i++ )
    {
        if( bankQueues[rankId][i].empty( ) == false )
        {
            rv = false;
            break;
        }
    }

    return rv;
}

/* 
 * MoveRankBank() increment curRank and/or curBank according to the scheduling
 * scheme
 * 0 -- Fixed Scheduling from Rank0 and Bank0
 * 1 -- Rank-first round-robin
 * 2 -- Bank-first round-robin
 */
void MemoryController::MoveRankBank( )
{
    if( p->ScheduleScheme == 1 )
    {
        /* increment Rank. if all ranks are looked, increment Bank then */
        curRank++;
        if( curRank == p->RANKS )
        {
            curRank = 0;
            curBank = (curBank + 1)%p->BANKS;
        }
    }
    else if( p->ScheduleScheme == 2 )
    {
        /* increment Bank. if all banks are looked, increment Rank then */
        curBank++;
        if( curBank == p->BANKS )
        {
            curBank = 0;
            curRank = (curRank + 1)%p->RANKS;
        }
    }

    /* if fixed scheduling is used, we do nothing */
}

void MemoryController::CalculateStats( )
{
    simulation_cycles = GetEventQueue()->GetCurrentCycle();

    GetChild( )->CalculateStats( );
    GetDecoder( )->CalculateStats( );
}
