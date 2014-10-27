/************************************************
 * Copyright (c) IBM Corp. 2014
 * This program and the accompanying materials
 * are made available under the terms of the Eclipse Public License v1.0
 * which accompanies this distribution, and is available at
 * http://www.eclipse.org/legal/epl-v10.html
 *************************************************/
/*
 * Contributors:
 *     arayshu, lschneid - initial implementation
 *     tjcw@us.ibm.com - enhancements
 */
/****
 * TODO:
 * 1. Handle data field on connect/accept
 * check . Freeing of LMRs
 * check . Completion for RDMA_Write 
 * check . Completion for Send
 * check . Completion for Recv
 * check . Data receiver thread
 * check . On new connection add socket fd to the select() array
 * check . Work Request Sender thread
 * check . Handle SGE lists that are larger then 1
 * check . Impl rdma_write
 ***/

#define _POSIX_C_SOURCE 201407L
#include <time.h>

#include <FxLogger.hpp>
#include <Histogram.hpp>
#include <ThreadSafeQueue.hpp>

#include <errno.h> // for perror()
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

#ifndef FXLOG_IT_API_O_SOCKETS
#define FXLOG_IT_API_O_SOCKETS ( 0 )
#endif

#ifndef FXLOG_IT_API_O_SOCKETS_CONNECT
#define FXLOG_IT_API_O_SOCKETS_CONNECT ( 0 )
#endif

#ifndef FXLOG_IT_API_O_SOCKETS_LOOP
#define FXLOG_IT_API_O_SOCKETS_LOOP ( 0 )
#endif

#ifndef FXLOG_IT_API_O_SOCKETS_QUEUE_LENGTHS_LOG
#define FXLOG_IT_API_O_SOCKETS_QUEUE_LENGTHS_LOG ( 0 )
#endif

#include <it_api_o_sockets_thread.h>

#include <mapepoll.h>

#define SPINNING_RECEIVE

pthread_mutex_t gITAPIFunctionMutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t gITAPI_INITMutex = PTHREAD_MUTEX_INITIALIZER;

pthread_mutex_t gAcceptThreadStartedMutex  = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t gSendThreadStartedMutex    = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t gReceiveThreadStartedMutex = PTHREAD_MUTEX_INITIALIZER;

int             gITAPI_Initialized = 0;

static itov_event_queue_t *gSendCmplQueue ;
static itov_event_queue_t *gRecvCmplQueue ;

int itov_aevd_defined = 0;
static void it_api_o_sockets_signal_accept(void) ;
/*******************************************************************/


it_status_t
socket_nonblock_on( int fd )
{
  int flags = fcntl( fd, F_GETFL);
  int rc = fcntl( fd, F_SETFL, flags | O_NONBLOCK);
  if (rc < 0)
    {
      BegLogLine( 1 )
        << "socket_nonblock_on(" << fd
        << "): ERROR: "
        << " errno: " << errno
        << EndLogLine;

      return IT_ERR_ABORT;
    }

  return IT_SUCCESS;
}

it_status_t
socket_nonblock_off( int fd )
{
  int flags = fcntl( fd, F_GETFL);
  int rc = fcntl( fd, F_SETFL, flags & ~O_NONBLOCK);
  if (rc < 0)
    {
      BegLogLine( 1 )
        << "socket_nonblock_off(" << fd
        << "): ERROR: "
        << " errno: " << errno
        << EndLogLine;

      return IT_ERR_ABORT;
    }

  return IT_SUCCESS;
}
it_status_t
socket_nodelay_on( int fd )
  {
    int one = 1 ;
    BegLogLine(FXLOG_IT_API_O_SOCKETS)
      << "Setting NODELAY for socket " << fd
      << EndLogLine ;
    int rc=setsockopt(fd, SOL_TCP, TCP_NODELAY, &one, sizeof(one)) ;
    if ( rc != 0 )
      {
        BegLogLine(1)
            << "Bad return from setsockopt fd=" << fd
            << " errno=" << errno
            << EndLogLine ;
      }
    return IT_SUCCESS ;
  }
typedef enum
  {
    IWARPEM_SUCCESS                 = 0x0001,
    IWARPEM_ERRNO_CONNECTION_RESET  = 0x0002,
    IWARPEM_ERRNO_CONNECTION_CLOSED = 0x0003
  } iWARPEM_Status_t ;

/***************************************
 * To enable IT_API over unix domain sockets
 * define IT_API_OVER_UNIX_DOMAIN_SOCKETS
 ***************************************/
//#define IT_API_OVER_UNIX_DOMAIN_SOCKETS

#ifdef IT_API_OVER_UNIX_DOMAIN_SOCKETS
#define IT_API_SOCKET_FAMILY AF_UNIX
#define IT_API_UNIX_SOCKET_DRC_PATH "/tmp/it_api_unix_control_socket"
#define IT_API_UNIX_SOCKET_PREFIX_PATH "/tmp/it_api_unix_socket"
#else
#define IT_API_SOCKET_FAMILY AF_INET
#endif
/***************************************/



#ifndef IT_API_REPORT_BANDWIDTH_ALL
#define IT_API_REPORT_BANDWIDTH_ALL ( 0 )
#endif

#ifndef IT_API_REPORT_BANDWIDTH_RDMA_WRITE_IN
#define IT_API_REPORT_BANDWIDTH_RDMA_WRITE_IN ( 0 | IT_API_REPORT_BANDWIDTH_ALL )
#endif

#ifndef IT_API_REPORT_BANDWIDTH_RDMA_READ_IN
#define IT_API_REPORT_BANDWIDTH_RDMA_READ_IN ( 0 | IT_API_REPORT_BANDWIDTH_ALL )
#endif

#ifndef IT_API_REPORT_BANDWIDTH_RECV
#define IT_API_REPORT_BANDWIDTH_RECV ( 0 | IT_API_REPORT_BANDWIDTH_ALL)
#endif

#ifndef IT_API_REPORT_BANDWIDTH_INCOMMING_TOTAL
#define IT_API_REPORT_BANDWIDTH_INCOMMING_TOTAL ( 1 | IT_API_REPORT_BANDWIDTH_ALL )
#endif

#ifndef IT_API_REPORT_BANDWIDTH_OUTGOING_TOTAL
#define IT_API_REPORT_BANDWIDTH_OUTGOING_TOTAL ( 1 | IT_API_REPORT_BANDWIDTH_ALL)
#endif

#define IT_API_REPORT_BANDWIDTH_DEFAULT_MODULO_BYTES (100*1024*1024)
#define IT_API_REPORT_BANDWIDTH_OUTGOING_MODULO_BYTES (1*1024*1024)

#define IT_API_SOCKET_BUFF_SIZE ( 1 * 1024 * 1024 )

struct iWARPEM_Bandwidth_Stats_t
{
  unsigned long long mTotalBytes;
  unsigned long long mBytesThisRound;
  unsigned long long mStartTime;
  unsigned long long mFirstStartTime;

  unsigned long long mReportLimit;
  
#define BANDWIDTH_STATS_CONTEXT_MAX_SIZE 256
  char               mContext[ BANDWIDTH_STATS_CONTEXT_MAX_SIZE ];
  
public:
  
  void
  Reset()
  {
    mBytesThisRound  = 0;
    mStartTime       = PkTimeGetNanos();    
  }

  void
  Init( const char* aContext, unsigned long long aReportLimit=IT_API_REPORT_BANDWIDTH_DEFAULT_MODULO_BYTES )
  {
    mReportLimit = aReportLimit;
    
    int ContextLen = strlen( aContext ) + 1; 
    StrongAssertLogLine( ContextLen < BANDWIDTH_STATS_CONTEXT_MAX_SIZE )
      << "ERROR: " 
      << " ContextLen: " << ContextLen
      << " BANDWIDTH_STATS_CONTEXT_MAX_SIZE: " << BANDWIDTH_STATS_CONTEXT_MAX_SIZE
      << EndLogLine;

    strcpy( mContext, aContext );
    
    mTotalBytes = 0;
    mFirstStartTime       = PkTimeGetNanos();    

    BegLogLine( FXLOG_IT_API_O_SOCKETS )
      << "iWARPEM_Bandwidth_Stats_t::Init(): "
      << " mContext: " << mContext
      << " mFirstStartTime: " << mFirstStartTime
      << EndLogLine;

    Reset();
  }

  void
  AddBytes( unsigned long long aBytes )
  {
    mBytesThisRound += aBytes;
    mTotalBytes     += aBytes;

    if( mBytesThisRound >= mReportLimit )
      ReportBandwidth();
  }
    
  void
  ReportBandwidth()
  {
    unsigned long long FinishTime = PkTimeGetNanos(); 

    double BandwidthThisRoundMB = ((mBytesThisRound * 1e9) / ( (FinishTime - mStartTime) )) / (1024.0 * 1024.0);

    double BandwidthAvgSinceStart = ((mTotalBytes * 1e9) / ( (FinishTime - mFirstStartTime) )) / (1024.0 * 1024.0);
    

    BegLogLine( (IT_API_REPORT_BANDWIDTH_OUTGOING_TOTAL|IT_API_REPORT_BANDWIDTH_INCOMMING_TOTAL) )
      << "iWARPEM_Bandwidth_Stats::ReportBandwidth(): "
      << " Context: " << mContext
      << " BandwidthThisRound (MB): " << BandwidthThisRoundMB
      << " BandwidthAvgSinceStart (MB): " << BandwidthAvgSinceStart
      << EndLogLine;
    
    Reset();
  }
};

iWARPEM_Bandwidth_Stats_t gBandInStat;
iWARPEM_Bandwidth_Stats_t gBandOutStat;

static 
inline 
iWARPEM_Status_t 
write_to_socket( int sock, char * buff, int len, int* wlen )
  {
    BegLogLine(FXLOG_IT_API_O_SOCKETS)
        << "Writing to FD=" << sock
        << " buff=" << (void *) buff
        << " length=" << len
        << EndLogLine ;
    int BytesWritten = 0;
    for( ; BytesWritten < len; )
    {
    int write_rc = write(   sock,
                            (((char *) buff) + BytesWritten ),
                            len - BytesWritten );
    if( write_rc < 0 )
      {
	// printf( "errno: %d\n", errno );
	if( errno == EAGAIN )
	  continue;
	else if ( errno == ECONNRESET )
	  {
	    return IWARPEM_ERRNO_CONNECTION_RESET;
	  }
	else
	  StrongAssertLogLine( 0 )
	    << "write_to_socket:: ERROR:: "
	    << "failed to write to file: " << sock
            << " buff: " << (void *) buff
            << " len: " << len
	    << " errno: " << errno
	    << EndLogLine;
      }

    BytesWritten += write_rc;
    }
  
  *wlen = BytesWritten;
  
#if IT_API_REPORT_BANDWIDTH_OUTGOING_TOTAL
  gBandOutStat.AddBytes( BytesWritten );
#endif

  return IWARPEM_SUCCESS;
  }

#ifndef IT_API_READ_FROM_SOCKET_HIST 
#define IT_API_READ_FROM_SOCKET_HIST ( 0 )
#endif

histogram_t<IT_API_READ_FROM_SOCKET_HIST> gReadCountHistogram;
histogram_t<IT_API_READ_FROM_SOCKET_HIST> gReadTimeHistogram;

static 
inline 
iWARPEM_Status_t 
read_from_socket( int sock, char * buff, int len, int* rlen )
  {
    BegLogLine(FXLOG_IT_API_O_SOCKETS)
        << "Reading from FD=" << sock
        << " buff=" << (void *) buff
        << " length=" << len
        << EndLogLine ;
    int BytesRead = 0;
    int ReadCount = 0;
    
    unsigned long long StartTime = PkTimeGetNanos();

    for(; BytesRead < len; )
      {
	int read_rc = read(   sock,
			      (((char *) buff) + BytesRead ),
			      len - BytesRead );
	if( read_rc < 0 )
	  {
	    // printf( "errno: %d\n", errno );
	    if( errno == EAGAIN || errno == EINTR )
	      continue;
	    else if ( errno == ECONNRESET )
	      {
	        BegLogLine(FXLOG_IT_API_O_SOCKETS)
	            << "ECONNRESET"
	            << EndLogLine ;
		return IWARPEM_ERRNO_CONNECTION_RESET;
	      }
	    else	      
	      StrongAssertLogLine( 0 )
		<< "read_from_socket:: ERROR:: "
		<< "failed to read from file: " << sock
		<< " errno: " << errno
    << " buff=" << (void *) buff
    << " " << (long) buff
    << " length=" << len
		<< EndLogLine;
	  }
	else if( read_rc == 0 )	  
	  {
	    BegLogLine(FXLOG_IT_API_O_SOCKETS)
	        << "Connection closed"
	        << EndLogLine ;
	  return IWARPEM_ERRNO_CONNECTION_CLOSED;
	  }
	
        ReadCount++;
        
	BytesRead += read_rc;
      }
    
    *rlen = BytesRead;

#if IT_API_REPORT_BANDWIDTH_INCOMMING_TOTAL
    gBandInStat.AddBytes( BytesRead );
#endif

    unsigned long long FinisTime = PkTimeGetNanos();

    gReadTimeHistogram.Add( FinisTime - StartTime );

    gReadCountHistogram.Add( ReadCount );
    
    static int Reported = 0;
    static long long ReportOnCount = 0;
    if( !Reported && ( ReportOnCount == 145489 ))
      {
        gReadTimeHistogram.Report();
        gReadCountHistogram.Report();
        Reported = 1;
      }

    ReportOnCount++;
    
    BegLogLine(FXLOG_IT_API_O_SOCKETS)
      << "Read completes"
      << EndLogLine ;
    return IWARPEM_SUCCESS;
  }


extern "C"
{
#define ITAPI_ENABLE_V21_BINDINGS
#include <it_api.h>
//#include "ordma_debug.h"
};

#ifndef IT_API_CHECKSUM
#define IT_API_CHECKSUM ( 0 )
#endif

struct iWARPEM_Private_Data_t
{
  int           mLen;
  unsigned char mData[ IT_MAX_PRIV_DATA ]; /* NFD-Moved - see it_event_cm_any_t */
};

struct iWARPEM_Object_MemoryRegion_t
  {
  it_pz_handle_t        pz_handle;
  void                 *addr;
  it_iobl_t             iobl;
  it_length_t           length;
  it_addr_mode_t        addr_mode;
  it_mem_priv_t         privs;
  it_lmr_flag_t         flags;
  uint32_t              shared_id;
  it_lmr_handle_t       lmr_handle;
  it_rmr_context_t      rmr_context;
  };

#if 0
 NOW COMES FROM /src/utils/ThreadSafeQueue.hpp
template< class Item >
struct TSafeQueue
{
  // new infrastrcture
  // for now, events will be held as a linked list. malloc'd.
  pthread_mutex_t         mutex;
  pthread_cond_t          empty_cond;

  Item *mHead;
  Item *mTail;
  int   mCount;
  int   mMax;

  pthread_mutex_t*
  GetMutex()
  {
    return &mutex;
  }

  void
  Finalize()
  {     
    Item* CurrentItem = mHead;
    
    Item* NextItem = NULL;
    if( mHead != NULL )
      NextItem = mHead->mNext;
    
    int i = mCount;

    while( i > 0 )
      {
	free( CurrentItem );
	  
	CurrentItem = NextItem;

	if( CurrentItem != NULL )
	  NextItem    = CurrentItem->mNext;

	i--;
      }
    
    mHead = NULL;
    mTail = NULL;

    pthread_mutex_destroy( & mutex );
    pthread_cond_destroy( & empty_cond );
  }

  void
  Init()
    {
    pthread_mutex_init( & mutex, NULL );
    pthread_cond_init( & empty_cond, NULL );

    mHead = NULL;
    mTail = NULL;
    mCount = 0;
    mMax   = 0;
    }

  int
  Enqueue( Item *aNextIn )
    {
    pthread_mutex_lock( &mutex );
    if( mCount == 0 )
      {
      AssertLogLine( mHead == NULL ) << "supposed to be empty" << EndLogLine;
      AssertLogLine( mTail == NULL ) << "supposed to be empty" << EndLogLine;
      mHead = aNextIn;
      mHead->mNext = NULL;      
      mHead->mPrev = NULL;      

      mTail = mHead;
      mCount = 1;
      }
    else
      {      
      mCount++;
      
      aNextIn->mPrev = mTail;
      mTail->mNext   = aNextIn;
      aNextIn->mNext = NULL;

      mTail = aNextIn;
      }
    pthread_mutex_unlock( &mutex );
    pthread_cond_broadcast( & empty_cond );
    return(0);
    }
  
  int 
  RemoveAssumeLocked( Item* aToRemove )
  {
    int rc;
    if( mCount == 0 )
      rc = -1;      
    else 
      {
	if( aToRemove->mNext != NULL )
	  aToRemove->mNext->mPrev = aToRemove->mPrev;

	if( aToRemove->mPrev != NULL )
	  aToRemove->mPrev->mNext = aToRemove->mNext;
	
	mCount--;

	if( mCount == 0 )
	  {
	    mHead = NULL;
	    mTail = NULL;
	  }	
	else if( aToRemove == mTail )
	  {
	    mTail = aToRemove->mPrev;
	  }
	else if( aToRemove == mHead )
	  {
	    mHead = mHead->mNext;
	  }
	rc = 0;
      }
    return rc;
  }

  int
  DequeueAssumedLockedNonEmpty( Item **aNextOut )
  {
    *aNextOut = mHead;
    
    mCount--;
    if( mCount == 0 )
      {
        mHead = NULL;
        mTail = NULL;
      }
    else
      {
        mHead = mHead->mNext;
	mHead->mPrev = NULL;
      }
    
    (*aNextOut)->mNext = NULL;
    (*aNextOut)->mPrev = NULL;
      
    return 0;
  }

  int
  DequeueAssumeLockedWithWait( Item **aNextOut )
  {
    while( mCount == 0 ) 
      pthread_cond_wait( & empty_cond, & mutex );

    return DequeueAssumedLockedNonEmpty( aNextOut );
  }

  int
  DequeueAssumeLocked( Item **aNextOut )
  {
    if( mCount == 0 )
      return -1;
    
    return DequeueAssumedLockedNonEmpty( aNextOut );
  }

  int
  DequeueWithWait( Item **aNextOut )
    {
      int rc;
      pthread_mutex_lock( &mutex );
      rc = DequeueAssumeLockedWithWait( aNextOut );
      pthread_mutex_unlock( &mutex );
      return(rc);
    }

  int
  Dequeue( Item **aNextOut )
    {
      int rc;
      pthread_mutex_lock( &mutex );
      rc = DequeueAssumeLocked( aNextOut );
      pthread_mutex_unlock( &mutex );
      return(rc);
    }
};

template< class Item >
struct TSafeDoubleLinkedList
{
  // new infrastructure
  // for now, events will be held as a linked list. malloc'd.
  pthread_mutex_t         mutex;
  Item *mHead;
  Item *mTail;
  int   mCount;
  Item *mCurrentItem;

  void
  Finalize()
  {     
    Item* CurrentItem = mHead;
    
    Item* NextItem = NULL;
    if( mHead != NULL )
      NextItem = mHead->mNext;
    
    int i = mCount;

    while( i > 0 )
      {
	free( CurrentItem );
	  
	CurrentItem = NextItem;

	if( CurrentItem != NULL )
	  NextItem    = CurrentItem->mNext;

	i--;
      }
    
    mHead = NULL;
    mTail = NULL;

    pthread_mutex_destroy( & mutex );
  }

  void
  Init()
    {
      BegLogLine( FXLOG_IT_API_O_SOCKETS )
	<< "TSafeDoubleLinkedList::Init(): Entering "
	<< EndLogLine;

    pthread_mutex_init( & mutex, NULL );
    mHead = NULL;
    mTail = NULL;
    mCurrentItem = NULL;
    mCount = 0;
    }

  int
  Insert( Item *aNextIn )
    {
      BegLogLine( FXLOG_IT_API_O_SOCKETS )
	<< "TSafeDoubleLinkedList::Insert(): Entering "
	<< " aNextIn: " << (void *) aNextIn
	<< EndLogLine;
      
    pthread_mutex_lock( &mutex );
    if( mCount == 0 )
      {
      AssertLogLine( mHead == NULL ) << "supposed to be empty" << EndLogLine;
      AssertLogLine( mTail == NULL ) << "supposed to be empty" << EndLogLine;
      aNextIn->mNext = NULL;
      aNextIn->mPrev = NULL;
      mHead = aNextIn;
      mTail = aNextIn;
      mCount = 1;
      }
    else
      {
      mCount++;
      
      mTail->mNext = aNextIn;
      aNextIn->mNext = NULL;
      aNextIn->mPrev = mTail;

      mTail = aNextIn;
      }
    pthread_mutex_unlock( &mutex );
    return(0);
    }  
  
  Item*
  GetNext()
  {
    pthread_mutex_lock( &mutex );
    
    if( mCurrentItem == NULL )
      {
	mCurrentItem = mHead;
	
	if( mCurrentItem == NULL )
	  {
	    pthread_mutex_unlock( &mutex );
	    return NULL;
	  }
      }
      
    Item* rc = mCurrentItem;

    mCurrentItem = mCurrentItem->mNext;
    
    pthread_mutex_unlock( &mutex );

    return rc;
  }

  int
  Remove( Item *aToRemove )
    {
    int rc;
    pthread_mutex_lock( &mutex );
    if( mCount == 0 )
      {
      rc = -1;
      }
    else
      {
	AssertLogLine( aToRemove != NULL )
	  << "TSafeDoubleLinkedList::Remove(): ERROR: "
	  << EndLogLine;
	
	if( aToRemove->mNext != NULL )
	  aToRemove->mNext->mPrev = aToRemove->mPrev;
	
	if( aToRemove->mPrev != NULL )
	  aToRemove->mPrev->mNext = aToRemove->mNext;
	
	mCount--;
	if( mCount == 0 )
	  {
	    mHead        = NULL;
	    mTail        = NULL;	
	    mCurrentItem = NULL;
	  }	
	else if( aToRemove == mTail )
	  mTail = aToRemove->mPrev;	
	else if( aToRemove == mHead )
	  mHead = aToRemove->mNext;
	
	if( mCurrentItem == aToRemove )
	  mCurrentItem = aToRemove->mNext;

	aToRemove->mNext = NULL;
	aToRemove->mPrev = NULL;
		
      rc = 0;
      }
    pthread_mutex_unlock( &mutex );
    return(rc);
    }
};
#endif


struct iWARPEM_Object_Event_t
  {
  it_event_t              mEvent;
//  iWARPEM_Object_Event_t *mNext;
//  iWARPEM_Object_Event_t *mPrev;
  };


#define IWARPEM_EVENT_QUEUE_MAX_SIZE ( 16384 )

#define IWARPEM_LOCKLESS_QUEUE 0

struct iWARPEM_Object_EventQueue_t
  {
  // from it_evd_create
  it_ia_handle_t   ia_handle;
  it_event_type_t  event_number;
  it_evd_flags_t   evd_flag;
  size_t           sevd_queue_size;
  size_t           sevd_threshold;
  it_evd_handle_t  aevd_handle;
  it_evd_handle_t  evd_handle;
  int              fd;

  // new infrastructure
  // for now, events will be held as a linked list. malloc'd.
  ThreadSafeQueue_t< iWARPEM_Object_Event_t *, IWARPEM_LOCKLESS_QUEUE > mQueue;

  void
  Init()
    {
      mQueue.Init( IWARPEM_EVENT_QUEUE_MAX_SIZE );
    }

  int
  Enqueue( iWARPEM_Object_Event_t *aNextIn )
    {
      AssertLogLine( aNextIn != NULL )
        << "iWARPEM_Object_EventQueue_t::Enqueue(): ERROR: "
        << " aNextIn: " << (void *) aNextIn
        << " mQueue@: " << (void *) & mQueue
        << EndLogLine;

      return mQueue.Enqueue( aNextIn );
    }

  int
  Dequeue( iWARPEM_Object_Event_t **aNextOut )
    {
      int rc = mQueue.Dequeue( aNextOut );
      
#ifndef NDEBUG
      if( rc != -1 )
        AssertLogLine( *aNextOut != NULL )
          << "iWARPEM_Object_EventQueue_t::Dequeue(): ERROR: "
          << " mQueue@: " << (void *) & mQueue
          << EndLogLine;
#endif      
      return rc;
    }
  };

typedef enum
  {
    IWARPEM_SOCKETCONTROL_TYPE_ADD    = 0x0001,
    IWARPEM_SOCKETCONTROL_TYPE_REMOVE = 0x0002
  } iWARPEM_SocketControl_Type_t;

struct iWARPEM_SocketControl_Hdr_t
{
  iWARPEM_SocketControl_Type_t mOpType;
  int                          mSockFd;
};

typedef enum
  {
    iWARPEM_DTO_SEND_TYPE = 1,
    iWARPEM_DTO_RECV_TYPE,
    iWARPEM_DTO_RDMA_WRITE_TYPE,    
    iWARPEM_DTO_RDMA_READ_REQ_TYPE,
    iWARPEM_DTO_RDMA_READ_RESP_TYPE,
    iWARPEM_DTO_RDMA_READ_CMPL_TYPE,
    iWARPEM_DISCONNECT_REQ_TYPE,
    iWARPEM_DISCONNECT_RESP_TYPE,
  } iWARPEM_Msg_Type_t;

static
const char* iWARPEM_Msg_Type_to_string( int /* iWARPEM_Msg_Type_t */ aType )
{
  switch( aType )
    {
    case iWARPEM_DTO_SEND_TYPE:           { return "iWARPEM_DTO_SEND_TYPE"; }
    case iWARPEM_DTO_RECV_TYPE:           { return "iWARPEM_DTO_RECV_TYPE"; }
    case iWARPEM_DTO_RDMA_WRITE_TYPE:     { return "iWARPEM_DTO_RDMA_WRITE_TYPE"; }
    case iWARPEM_DTO_RDMA_READ_REQ_TYPE:  { return "iWARPEM_DTO_RDMA_READ_REQ_TYPE"; }
    case iWARPEM_DTO_RDMA_READ_RESP_TYPE: { return "iWARPEM_DTO_RDMA_READ_RESP_TYPE"; }
    case iWARPEM_DTO_RDMA_READ_CMPL_TYPE: { return "iWARPEM_DTO_RDMA_READ_CMPL_TYPE"; }
    case iWARPEM_DISCONNECT_REQ_TYPE:     { return "iWARPEM_DISCONNECT_REQ_TYPE"; }
    case iWARPEM_DISCONNECT_RESP_TYPE:    { return "iWARPEM_DISCONNECT_RESP_TYPE"; }
    default:
      {
	StrongAssertLogLine( 0 )
	  << "iWARPEM_Msg_Type_to_string(): "
	  << " aType: " << aType
	  << EndLogLine;
      } 
    }
  
  return NULL;
}

struct iWARPEM_RDMA_Write_t
{
  // Addr  (absolute or relative) in the memory region
  // on the remote host
  it_rdma_addr_t   mRMRAddr;
  
  // Pointer to a memory region
  it_rmr_context_t mRMRContext;
};

struct iWARPEM_RDMA_Read_Req_t
{
  // Addr  (absolute or relative) in the memory region
  // on the remote host
  it_rdma_addr_t                 mRMRAddr;
  
  // Pointer to a memory region
  it_rmr_context_t               mRMRContext;

  int                            mDataToReadLen;

  void *                         mPrivatePtr; // iWARPEM_Object_WorkRequest_t * mRdmaReadClientWorkRequestState;
};

struct iWARPEM_RDMA_Read_Resp_t
{
  void *                         mPrivatePtr; // iWARPEM_Object_WorkRequest_t * mRdmaReadClientWorkRequestState;
};

struct iWARPEM_Send_t
{
  // place holder
};

struct iWARPEM_Recv_t
{
  // place holder
};

struct iWARPEM_Message_Hdr_t
{
  iWARPEM_Msg_Type_t     mMsg_Type;
  int                    mTotalDataLen;

#if IT_API_CHECKSUM
  uint64_t               mChecksum;
#endif

  union 
  {
    iWARPEM_RDMA_Write_t      mRdmaWrite;
    iWARPEM_RDMA_Read_Req_t   mRdmaReadReq;
    iWARPEM_RDMA_Read_Resp_t  mRdmaReadResp;
    iWARPEM_Send_t            mSend;
    iWARPEM_Recv_t            mRecv;
  } mOpType;
  void EndianConvert(void)
    {
      mMsg_Type=(iWARPEM_Msg_Type_t)ntohl(mMsg_Type) ;
      BegLogLine(FXLOG_IT_API_O_SOCKETS)
        << "mMsg_Type endian converted to " << mMsg_Type
        << EndLogLine ;
    }
};

struct iWARPEM_Object_WorkRequest_t
  {    
  iWARPEM_Message_Hdr_t  mMessageHdr;

  // Local from it_post_send
  it_ep_handle_t    ep_handle;
  it_lmr_triplet_t *segments_array;
  size_t            num_segments;
  it_dto_cookie_t   cookie;
  it_dto_flags_t    dto_flags;

//   iWARPEM_Object_WorkRequest_t *mNext;
//   iWARPEM_Object_WorkRequest_t *mPrev;
  };

it_status_t iwarpem_it_post_rdma_read_resp ( 
  IN int                                SocketFd,
  IN it_lmr_triplet_t*                  LocalSegment,
  IN void*                              RdmaReadClientWorkRequestState 
  );

it_status_t iwarpem_generate_rdma_read_cmpl_event( iWARPEM_Object_WorkRequest_t * aSendWR );


#define IWARPEM_SEND_WR_QUEUE_MAX_SIZE 65536
#define IWARPEM_RECV_WR_QUEUE_MAX_SIZE 8192

struct iWARPEM_Object_WR_Queue_t
{
  ThreadSafeQueue_t< iWARPEM_Object_WorkRequest_t *, IWARPEM_LOCKLESS_QUEUE > mQueue;

  void
  Finalize()
  {
    mQueue.Finalize();    
  }

  void
  Init(int aSize)
    {
      mQueue.Init( aSize );
    }
  
  pthread_mutex_t*
  GetMutex()
  {
    return mQueue.GetMutex();
  }

  int
  Enqueue( iWARPEM_Object_WorkRequest_t *aNextIn )
    {
      return mQueue.Enqueue( aNextIn );
    }

  int
  Dequeue( iWARPEM_Object_WorkRequest_t **aNextOut )
    {
      return mQueue.Dequeue( aNextOut );
    }

  int
  DequeueAssumeLocked( iWARPEM_Object_WorkRequest_t **aNextOut )
    {
      return mQueue.DequeueAssumeLocked( aNextOut );
    }
};

iWARPEM_Object_WR_Queue_t* gSendWrQueue = NULL;


unsigned int               gBlockedFlag = 0;
pthread_mutex_t            gBlockMutex;
pthread_cond_t             gBlockCond;
iWARPEM_Object_WR_Queue_t* gRecvToSendWrQueue = NULL;

void
iwarpem_enqueue_send_wr( iWARPEM_Object_WR_Queue_t* aQueue, iWARPEM_Object_WorkRequest_t * aSendWr )
{
  
  if( gBlockedFlag )
    {
      pthread_cond_broadcast( &gBlockCond );      
    }
  
  int enqrc = aQueue->Enqueue( aSendWr );

  AssertLogLine( enqrc == 0 )
    << "iwarpem_enqueue_send_wr(): ERROR:: "
    << " enqrc: " << enqrc
    << EndLogLine;
}

typedef enum
  {
    IWARPEM_CONNECTION_FLAG_DISCONNECTED                       = 0x0000,
    IWARPEM_CONNECTION_FLAG_CONNECTED                          = 0x0001,
    IWARPEM_CONNECTION_FLAG_ACTIVE_SIDE_PENDING_DISCONNECT     = 0x0002,
    IWARPEM_CONNECTION_FLAG_PASSIVE_SIDE_PENDING_DISCONNECT    = 0x0003
  } iWARPEM_Connection_Flags_t;

// The EndPoint is here to so top down declarations work
struct iWARPEM_Object_EndPoint_t
  {
  // it_ep_rc_create args
  it_pz_handle_t            pz_handle;
  it_evd_handle_t           request_sevd_handle;
  it_evd_handle_t           recv_sevd_handle;
  it_evd_handle_t           connect_sevd_handle;
  it_ep_rc_creation_flags_t flags;
  it_ep_attributes_t        ep_attr;
  it_ep_handle_t            ep_handle;

  // added infrastructure
  iWARPEM_Connection_Flags_t  ConnectedFlag;
  int                         ConnFd;
  int                         OtherEPNodeId;
    
  iWARPEM_Object_WR_Queue_t   RecvWrQueue;

    // Send for sends and writes    
//   iWARPEM_Object_EndPoint_t* mNext;
//   iWARPEM_Object_EndPoint_t* mPrev;
  };

template <class streamclass>
static streamclass& operator<<( streamclass& os, iWARPEM_Object_EndPoint_t &aArg ) 
{
  os << "EP@ " << (void *) aArg.ep_handle; 
  os << " [ ";
  os << " OEPNodeId: " << aArg.OtherEPNodeId << " ";
  os << " sock: " << aArg.ConnFd << " ";
  os << " ]";

  return os;
}


it_status_t iwarpem_it_ep_disconnect_resp ( iWARPEM_Object_EndPoint_t* aLocalEndPoint );

#define                    SOCK_FD_TO_END_POINT_MAP_COUNT ( 8192 )
iWARPEM_Object_EndPoint_t* gSockFdToEndPointMap[ SOCK_FD_TO_END_POINT_MAP_COUNT ];

//TSafeDoubleLinkedList<iWARPEM_Object_EndPoint_t> gSendWRLocalEndPointList;

// U it_ia_create

// ia_handles are really void*s -- easy to hand out counting numbers
static int ape_ia_handle_next = 0;

struct DataReceiverThreadArgs
{
  int                drc_cli_socket;

#ifdef IT_API_OVER_UNIX_DOMAIN_SOCKETS
  struct sockaddr_un drc_cli_addr;
#else
  struct sockaddr_in drc_cli_addr;
#endif
};

DataReceiverThreadArgs gDataReceiverThreadArgs;

typedef enum
  {
    IWARPEM_FLUSH_SEND_QUEUE_FLAG = 0x0001,
    IWARPEM_FLUSH_RECV_QUEUE_FLAG = 0x0002
  } iwarpem_flush_queue_flag_t;

void 
iwarpem_flush_queue( iWARPEM_Object_EndPoint_t* aEndPoint,
		     iwarpem_flush_queue_flag_t aFlag )
{
  //  BegLogLine( FXLOG_IT_API_O_SOCKETS )
  BegLogLine( FXLOG_IT_API_O_SOCKETS )
    << "iwarpem_flush_queue(): Entering... "
    << " aEndPoint: " << (void *) aEndPoint
    << " aFlag: " << aFlag
    << EndLogLine;

  /********************************************************
   * Flush the send queue associated with this end point
   ********************************************************/
  pthread_mutex_t* MutexPtr = NULL;

  int Send = ( aFlag == IWARPEM_FLUSH_SEND_QUEUE_FLAG );
  int Recv = ( aFlag == IWARPEM_FLUSH_RECV_QUEUE_FLAG );

  // Using lock free queue
#if 0  
  if( Send )
    MutexPtr = gSendWrQueue->GetMutex();
  else
    MutexPtr = aEndPoint->RecvWrQueue.GetMutex();
    
  pthread_mutex_lock( MutexPtr );
#endif
  
  iWARPEM_Object_WorkRequest_t* WR;

  if( Send )
    {      
#if 1
      // Flush the gSendWrQueue queue      
      int Start = gSendWrQueue->mQueue.mGotCount;
      int End   = gSendWrQueue->mQueue.mPutCount;

      for( int i = Start; i < End; i++ )
        {
          int ItemIndex = i & gSendWrQueue->mQueue.mDepthMask;
          WR = gSendWrQueue->mQueue.mItemArray[ ItemIndex ];
          
          if( (iWARPEM_Object_EndPoint_t *) WR->ep_handle == aEndPoint )
            {
              gSendWrQueue->mQueue.mItemArray[ ItemIndex ] = NULL;
            }
          else 
            continue;
          
	  iWARPEM_Object_Event_t* DTOCompletetionEvent = 
	    (iWARPEM_Object_Event_t*) malloc( sizeof( iWARPEM_Object_Event_t ) );
      
	  it_dto_cmpl_event_t* dtoce = (it_dto_cmpl_event_t*) & DTOCompletetionEvent->mEvent;  

	  if( WR->mMessageHdr.mMsg_Type == iWARPEM_DTO_SEND_TYPE )
	    dtoce->event_number = IT_DTO_SEND_CMPL_EVENT;
	  else if( WR->mMessageHdr.mMsg_Type == iWARPEM_DTO_RDMA_WRITE_TYPE )
	    dtoce->event_number = IT_DTO_RDMA_WRITE_CMPL_EVENT;
	  else if( WR->mMessageHdr.mMsg_Type == iWARPEM_DTO_RDMA_READ_REQ_TYPE )
	    dtoce->event_number = IT_DTO_RDMA_READ_CMPL_EVENT;
	  else 
	    StrongAssertLogLine( 0 ) 
	      << "iwarpem_flush_queue(): ERROR:: "
	      << " WR->mMessageHdr.mMsg_Type: " << WR->mMessageHdr.mMsg_Type
	      << EndLogLine;
	  
	  dtoce->evd          = aEndPoint->request_sevd_handle;
	  
	  dtoce->ep           = (it_ep_handle_t) aEndPoint;
	  dtoce->cookie       = WR->cookie;
	  dtoce->dto_status   = IT_DTO_ERR_FLUSHED;
	  dtoce->transferred_length = 0;
	  
	  iWARPEM_Object_EventQueue_t* CmplEventQueue = 
	    (iWARPEM_Object_EventQueue_t*) dtoce->evd;
	  
	  int enqrc = CmplEventQueue->Enqueue( DTOCompletetionEvent );

          AssertLogLine( enqrc == 0 )
            << "iwarpem_flush_queue(): ERROR:: "
            << " enqrc: " << enqrc
            << EndLogLine;
          
          // Free WR resources          
          if( WR->segments_array )
            free( WR->segments_array );
          
          free( WR );          
	}

      // Flush the RecvToSendQueue
      Start = gRecvToSendWrQueue->mQueue.mGotCount;
      End   = gRecvToSendWrQueue->mQueue.mPutCount;

      for( int i = Start; i < End; i++ )
        {
          int ItemIndex = i & gRecvToSendWrQueue->mQueue.mDepthMask;
          WR = gRecvToSendWrQueue->mQueue.mItemArray[ ItemIndex ];
          
          if( (iWARPEM_Object_EndPoint_t *) WR->ep_handle == aEndPoint )
            {
              gRecvToSendWrQueue->mQueue.mItemArray[ ItemIndex ] = NULL;
            }
          else 
            continue;
          
          if( WR->mMessageHdr.mMsg_Type == iWARPEM_DTO_RDMA_READ_CMPL_TYPE )
            {                        
              iWARPEM_Object_Event_t* DTOCompletetionEvent = 
                (iWARPEM_Object_Event_t*) malloc( sizeof( iWARPEM_Object_Event_t ) );
              
              it_dto_cmpl_event_t* dtoce = (it_dto_cmpl_event_t*) & DTOCompletetionEvent->mEvent;  
              
              dtoce->event_number = IT_DTO_RDMA_READ_CMPL_EVENT;
              dtoce->evd          = aEndPoint->request_sevd_handle;
              
              dtoce->ep           = (it_ep_handle_t) aEndPoint;
              dtoce->cookie       = WR->cookie;
              dtoce->dto_status   = IT_DTO_ERR_FLUSHED;
              dtoce->transferred_length = 0;
              
              iWARPEM_Object_EventQueue_t* CmplEventQueue = 
                (iWARPEM_Object_EventQueue_t*) dtoce->evd;
              
              int enqrc = CmplEventQueue->Enqueue( DTOCompletetionEvent );

              AssertLogLine( enqrc == 0 )
                << "iwarpem_flush_queue(): ERROR:: "
                << " enqrc: " << enqrc
                << EndLogLine;              
            }
          
          if( WR->segments_array )
            free( WR->segments_array );

          free( WR );
        }
#else
      WR = gSendWrQueue->mQueue.mHead;
      
      while( WR != NULL )
	{
	  if( (iWARPEM_Object_EndPoint_t *) WR->ep_handle == aEndPoint )
	    {
	      gSendWrQueue->mQueue.RemoveAssumeLocked( WR );
	    }
	  else 
	    {
	      WR = WR->mNext;
	      continue;
	    }

	  // Don't generate completions for the internal RDMA_READ_RESP_TYPE
	  if( WR->mMessageHdr.mMsg_Type == iWARPEM_DTO_RDMA_READ_RESP_TYPE ) 
	    {
	      WR = WR->mNext;
	      continue;	  
	    }

	  iWARPEM_Object_Event_t* DTOCompletetionEvent = 
	    (iWARPEM_Object_Event_t*) malloc( sizeof( iWARPEM_Object_Event_t ) );
      
	  it_dto_cmpl_event_t* dtoce = (it_dto_cmpl_event_t*) & DTOCompletetionEvent->mEvent;  

	  if( WR->mMessageHdr.mMsg_Type == iWARPEM_DTO_SEND_TYPE )
	    dtoce->event_number = IT_DTO_SEND_CMPL_EVENT;
	  else if( WR->mMessageHdr.mMsg_Type == iWARPEM_DTO_RDMA_WRITE_TYPE )
	    dtoce->event_number = IT_DTO_RDMA_WRITE_CMPL_EVENT;
	  else if( WR->mMessageHdr.mMsg_Type == iWARPEM_DTO_RDMA_READ_REQ_TYPE )
	    dtoce->event_number = IT_DTO_RDMA_READ_CMPL_EVENT;
	  else 
	    StrongAssertLogLine( 0 ) 
	      << "iwarpem_flush_queue(): ERROR:: "
	      << " WR->mMessageHdr.mMsg_Type: " << WR->mMessageHdr.mMsg_Type
	      << EndLogLine;
	  
	  dtoce->evd          = aEndPoint->request_sevd_handle;
	  
	  dtoce->ep           = (it_ep_handle_t) aEndPoint;
	  dtoce->cookie       = WR->cookie;
	  dtoce->dto_status   = IT_DTO_ERR_FLUSHED;
	  dtoce->transferred_length = 0;
	  
	  iWARPEM_Object_EventQueue_t* CmplEventQueue = 
	    (iWARPEM_Object_EventQueue_t*) dtoce->evd;
	  
	  int enqrc = CmplEventQueue->Enqueue( DTOCompletetionEvent );
	  	  
	  WR = WR->mNext;
	}
#endif
    }
  else 
    {
      while( 1 )
	{
	  int status = -1;
	  
	  status = aEndPoint->RecvWrQueue.DequeueAssumeLocked( &WR );
	  
	  if( status == -1 ) 
	    break;	  

	  iWARPEM_Object_Event_t* DTOCompletetionEvent = 
	    (iWARPEM_Object_Event_t*) malloc( sizeof( iWARPEM_Object_Event_t ) );
      
	  it_dto_cmpl_event_t* dtoce = (it_dto_cmpl_event_t*) & DTOCompletetionEvent->mEvent;  
	  
	  dtoce->event_number = IT_DTO_RC_RECV_CMPL_EVENT;
	  dtoce->evd          = aEndPoint->recv_sevd_handle; 
	
	  dtoce->ep           = (it_ep_handle_t) aEndPoint;
	  dtoce->cookie       = WR->cookie;
	  dtoce->dto_status   = IT_DTO_ERR_FLUSHED;
	  dtoce->transferred_length = 0;
	  
	  iWARPEM_Object_EventQueue_t* CmplEventQueue = 
	    (iWARPEM_Object_EventQueue_t*) dtoce->evd;
	  
	  int enqrc = CmplEventQueue->Enqueue( DTOCompletetionEvent );
	  
	  StrongAssertLogLine( enqrc == 0 ) 
	    << "iwarpem_flush_queue(): ERROR:: failed to enqueue connection request event" 
	    << EndLogLine;
          
          // Free the WR resources          
          if( WR->segments_array )
            free( WR->segments_array );
          
          free( WR );
	}
    }

  // Using lock free queue
#if 0
  pthread_mutex_unlock( MutexPtr );
#endif
  /********************************************************/

  // BegLogLine( FXLOG_IT_API_O_SOCKETS )
  BegLogLine( FXLOG_IT_API_O_SOCKETS )
    << "iwarpem_flush_queue(): Leaving "
    << EndLogLine;

  return;
}

pthread_mutex_t gGenerateConnTerminationEventMutex;

void
iwarpem_generate_conn_termination_event( int aSocketId )
{
  BegLogLine( FXLOG_IT_API_O_SOCKETS )
    << "iwarpem_generate_conn_termination_event(): Entering "
    << " aSocketId: " << aSocketId
    << EndLogLine;

  pthread_mutex_lock( & gGenerateConnTerminationEventMutex ); 

  AssertLogLine( aSocketId >= 0 && aSocketId < SOCK_FD_TO_END_POINT_MAP_COUNT )
    << "iwarpem_generate_conn_termination_event(): "
    << " aSocketId: " << aSocketId
    << EndLogLine;

  iWARPEM_Object_EndPoint_t* LocalEndPoint = gSockFdToEndPointMap[ aSocketId ];
  if( LocalEndPoint == NULL )
    {
    pthread_mutex_unlock( & gGenerateConnTerminationEventMutex );   
    return;
    }

  if( LocalEndPoint->ConnectedFlag != IWARPEM_CONNECTION_FLAG_PASSIVE_SIDE_PENDING_DISCONNECT )
    {
      // If we have a broken connection
#if 0      
      iwarpem_flush_queue( LocalEndPoint, IWARPEM_FLUSH_SEND_QUEUE_FLAG );
      iwarpem_flush_queue( LocalEndPoint, IWARPEM_FLUSH_RECV_QUEUE_FLAG );
#endif      
      // gSendWRLocalEndPointList.Remove( LocalEndPoint );      
    } 
  
  BegLogLine( FXLOG_IT_API_O_SOCKETS )
    << "iwarpem_generate_conn_termination_event(): Before close() "
    << " aSocketId: " << aSocketId
    << EndLogLine;

  close( aSocketId );

  BegLogLine( FXLOG_IT_API_O_SOCKETS )
    << "iwarpem_generate_conn_termination_event(): After close() "
    << " aSocketId: " << aSocketId
    << EndLogLine;

  gSockFdToEndPointMap[ aSocketId ] = NULL;
  
  /**********************************************
   * Generate send completion event
   *********************************************/
  /* TODO: These look leaked */
  iWARPEM_Object_Event_t* CompletetionEvent = 
    (iWARPEM_Object_Event_t*) malloc( sizeof( iWARPEM_Object_Event_t ) );
  BegLogLine(FXLOG_IT_API_O_SOCKETS)
    << "CompletetionEvent malloc -> " << (void *) CompletetionEvent
    << EndLogLine ;
  
  it_connection_event_t* conne = (it_connection_event_t *) & CompletetionEvent->mEvent;  
  
  if( LocalEndPoint->ConnectedFlag == IWARPEM_CONNECTION_FLAG_PASSIVE_SIDE_PENDING_DISCONNECT )
    conne->event_number = IT_CM_MSG_CONN_DISCONNECT_EVENT;
  else 
    conne->event_number = IT_CM_MSG_CONN_BROKEN_EVENT;
  
  conne->evd          = LocalEndPoint->connect_sevd_handle;
  conne->ep           = (it_ep_handle_t) LocalEndPoint;		    
  
  iWARPEM_Object_EventQueue_t* ConnCmplEventQueue = 
    (iWARPEM_Object_EventQueue_t*) conne->evd;
  
  int enqrc = ConnCmplEventQueue->Enqueue( CompletetionEvent );
  
  BegLogLine(FXLOG_IT_API_O_SOCKETS)
    << "ConnCmplEventQueue=" << ConnCmplEventQueue
    << " conne->event_number=" << conne->event_number
    << " conne->evd=" << conne->evd
    << " conne->ep=" << conne->ep
    << EndLogLine ;

  StrongAssertLogLine( enqrc == 0 ) 
    << "iWARPEM_DataReceiverThread()::Failed to enqueue connection request event" 
    << EndLogLine;	  
  /*********************************************/
  
  LocalEndPoint->ConnectedFlag = IWARPEM_CONNECTION_FLAG_DISCONNECTED;

  pthread_mutex_unlock( & gGenerateConnTerminationEventMutex ); 

  BegLogLine( FXLOG_IT_API_O_SOCKETS )
    << "iwarpem_generate_conn_termination_event(): Leaving "
    << " aSocketId: " << aSocketId
    << EndLogLine;
}

void*
iWARPEM_DataReceiverThread( void* args )
{
  BegLogLine( FXLOG_IT_API_O_SOCKETS )
    << "iWARPEM_DataReceiverThread(): Started!"
    << EndLogLine;

  pthread_mutex_unlock( & gReceiveThreadStartedMutex );

  DataReceiverThreadArgs* DataReceiverThreadArgsPtr = (DataReceiverThreadArgs *) args;

  /**********************************************
   * Connect to the listen thread
   **********************************************/
  int drc_client_socket = DataReceiverThreadArgsPtr->drc_cli_socket;

// Following chunk now done in mainline; a connected socket is passed in
//  BegLogLine(FXLOG_IT_API_O_SOCKETS)
//    << "iWARPEM_DataReceiverThread:: Before connect()"
//    << " drc_client_socket: " << drc_client_socket
//    << EndLogLine;
//
//  while( 1 )
//    {
//      int conn_rc = connect( drc_client_socket,
//			     (struct sockaddr *) & (DataReceiverThreadArgsPtr->drc_cli_addr),
//			     sizeof( DataReceiverThreadArgsPtr->drc_cli_addr ) );
//
//      if( conn_rc == 0 )
//	break;
//      else if( conn_rc < 0 )
//	{
//	  if( errno != EAGAIN )
//	    {
//	      perror( "connect failed" );
//	      StrongAssertLogLine( 0 )
//		<< "iWARPEM_DataReceiverThread:: Error after connect(): "
//		<< " errno: " << errno
//		<< " conn_rc: " << conn_rc
//		<< EndLogLine;
//	    }
//	}
//    }
#if defined(SPINNING_RECEIVE)
  socket_nonblock_on(drc_client_socket) ;
#endif
  
  BegLogLine(FXLOG_IT_API_O_SOCKETS)
    << "iWARPEM_DataReceiverThread:: After connect()"
    << " drc_client_socket: " << drc_client_socket
    << EndLogLine;  
  /**********************************************/



  /**********************************************
   * Setup epoll
   **********************************************/  
#define MAX_EPOLL_FD ( 2048 )

  mapepfd_t epoll_fd = mapepoll_create( MAX_EPOLL_FD );
  StrongAssertLogLine (epoll_fd >= 0 )
    << "iWARPEM_DataReceiverThread:: epoll_create() failed"      
    << " errno: " << errno
    << EndLogLine;
  
  struct epoll_event EP_Event;
  EP_Event.events = EPOLLIN;
  EP_Event.data.fd = drc_client_socket;
  
  BegLogLine( FXLOG_IT_API_O_SOCKETS )
    << "iWARPEM_DataReceiverThread:: About to mapepoll_ctl"
    << " drc_client_socket: " << drc_client_socket
    << " EP_Event.data.fd: " << EP_Event.data.fd
    << EndLogLine;


  int mapepoll_ctl_rc = mapepoll_ctl( epoll_fd,
				EPOLL_CTL_ADD, 
				drc_client_socket,
				& EP_Event );

  StrongAssertLogLine( mapepoll_ctl_rc == 0 )
    << "iWARPEM_DataReceiverThread:: mapepoll_ctl() failed"
    << " errno: " << errno
    << EndLogLine;        

  struct epoll_event* InEvents = (struct epoll_event *) malloc( sizeof( struct epoll_event ) * MAX_EPOLL_FD );
  StrongAssertLogLine( InEvents )
    << "iWARPEM_DataReceiverThread:: malloc() failed"      
    << EndLogLine;
  /**********************************************/

  while( 1 )
    {
      int event_num = mapepoll_wait( epoll_fd,
				  InEvents,
				  MAX_EPOLL_FD,
				  100 ); // in milliseconds
      
      if( event_num == -1 )
        {
          if( errno == EINTR )
            continue;
        }

      AssertLogLine( event_num != -1 )
	<< "iWARPEM_DataReceiverThread:: ERROR:: "      
	<< " errno: " << errno
	<< EndLogLine;

      for( int i = 0; i < event_num; i++ )
	{
	  int SocketFd = InEvents[ i ].data.fd;

	  StrongAssertLogLine(InEvents[ i ].events & EPOLLIN)
	    << "iWARPEM_DataReceiverThread:: ERROR:: "      
	    << " i: " << i
	    << " InEvents[ i ].events: " << (void *)InEvents[ i ].events
	    << " SocketFd: " << SocketFd
	    << EndLogLine;

	  BegLogLine(FXLOG_IT_API_O_SOCKETS)
      << " i: " << i
      << " InEvents[ i ].events: " << (void *)InEvents[ i ].events
      << " SocketFd: " << SocketFd
	    << EndLogLine ;

	  if( ( InEvents[ i ].events & EPOLLERR ) ||
	      ( InEvents[ i ].events & EPOLLHUP ) )
	    {
	      BegLogLine(FXLOG_IT_API_O_SOCKETS)
	          << "Error or hangup"
	          << EndLogLine ;

	      iWARPEM_Object_EndPoint_t* LocalEndPoint = gSockFdToEndPointMap[ SocketFd ];

	      struct epoll_event EP_Event;
	      EP_Event.events = EPOLLIN;
	      EP_Event.data.fd = SocketFd;
		  
	      int mapepoll_ctl_rc = mapepoll_ctl( epoll_fd,
					    EPOLL_CTL_DEL, 
					    SocketFd,
					    & EP_Event );
		  
	      StrongAssertLogLine( mapepoll_ctl_rc == 0 )
		<< "iWARPEM_DataReceiverThread:: mapepoll_ctl() failed"
		<< " errno: " << errno
		<< EndLogLine;		    

	      iwarpem_generate_conn_termination_event( SocketFd );
	      
	      continue;
	    }
	  
	  if( SocketFd == drc_client_socket )
	    {
	      // Control socket
	      iWARPEM_SocketControl_Hdr_t ControlHdr;
	      int rlen;
	      iWARPEM_Status_t istatus = read_from_socket( SocketFd,
							   (char *) & ControlHdr,
							   sizeof( iWARPEM_SocketControl_Hdr_t ),
							   & rlen );
	      
	      StrongAssertLogLine( istatus == IWARPEM_SUCCESS )
		<< "iWARPEM_DataReceiverThread(): ERROR:: "		
		<< " istatus: " << istatus
		<< EndLogLine;
	      
	      int SockFd = ControlHdr.mSockFd;
	      
	      struct epoll_event EP_Event;
	      EP_Event.events = EPOLLIN;
	      EP_Event.data.fd = SockFd;
	      
	      BegLogLine( FXLOG_IT_API_O_SOCKETS )
		<< "iWARPEM_DataReceiverThread:: About to mapepoll_ctl"
		<< " SocketFd: " << SocketFd
		<< " ControlHdr.mSockFd: " << ControlHdr.mSockFd
		<< EndLogLine;

	      switch( ControlHdr.mOpType )
		{
		case IWARPEM_SOCKETCONTROL_TYPE_ADD:
		  {
		    BegLogLine(FXLOG_IT_API_O_SOCKETS)
		        << "Adding socket " << SockFd
		        << " to poll"
		        << EndLogLine ;
		    int mapepoll_ctl_rc = mapepoll_ctl( epoll_fd,
						  EPOLL_CTL_ADD, 
						  SockFd,
						  & EP_Event );
		    
		    StrongAssertLogLine( mapepoll_ctl_rc == 0 )
		      << "iWARPEM_DataReceiverThread:: mapepoll_ctl() failed"
		      << " errno: " << errno
		      << EndLogLine;	      
		    
		    break;
		  }
		case IWARPEM_SOCKETCONTROL_TYPE_REMOVE:
		  {
        BegLogLine(FXLOG_IT_API_O_SOCKETS)
            << "Removing socket " << SockFd
            << " from poll"
            << EndLogLine ;
		    int mapepoll_ctl_rc = mapepoll_ctl( epoll_fd,
						  EPOLL_CTL_DEL, 
						  SockFd,
						  & EP_Event );
		    
		    StrongAssertLogLine( mapepoll_ctl_rc == 0 )
		      << "iWARPEM_DataReceiverThread:: mapepoll_ctl() failed"
		      << " errno: " << errno
		      << EndLogLine;	      
		    
		    break;
		  }
		default:
		  StrongAssertLogLine( 0 )
		    << "iWARPEM_DataReceiverThread:: ERROR:: "
		    << " ControlHdr.mOpType: " << ControlHdr.mOpType
		    << EndLogLine;
		}	      

	    }
	  else
	    {
	      iWARPEM_Message_Hdr_t Hdr;
	      int rlen_expected;
	      int rlen = sizeof( iWARPEM_Message_Hdr_t );
	      iWARPEM_Status_t istatus = read_from_socket( SocketFd,
							   (char *) &Hdr,
							   rlen,
							   & rlen_expected );

	      if( istatus != IWARPEM_SUCCESS || ( rlen != rlen_expected ) )
		{
		  struct epoll_event EP_Event;
		  EP_Event.events = EPOLLIN;
		  EP_Event.data.fd = SocketFd;
		  
		  int mapepoll_ctl_rc = mapepoll_ctl( epoll_fd,
						EPOLL_CTL_DEL, 
						SocketFd,
						& EP_Event );
		  
		  StrongAssertLogLine( mapepoll_ctl_rc == 0 )
		    << "iWARPEM_DataReceiverThread:: mapepoll_ctl() failed"
		    << " errno: " << errno
		    << EndLogLine;		    
		  
		  iwarpem_generate_conn_termination_event( SocketFd );
		  continue;
		}
//	      Hdr.mMsg_Type=ntohl(Hdr.mMsg_Type) ;
	      Hdr.EndianConvert() ;
	      Hdr.mTotalDataLen=ntohl(Hdr.mTotalDataLen) ;

	      BegLogLine( FXLOG_IT_API_O_SOCKETS )
		<< "iWARPEM_DataReceiverThread:: read_from_socket() for header"
		<< " SocketFd: " << SocketFd
		<< " Hdr.mMsg_Type: " << Hdr.mMsg_Type
		<< " Hdr.mTotalDataLen: " << Hdr.mTotalDataLen
		<< EndLogLine;
	      
	      switch( Hdr.mMsg_Type )
		{
		case iWARPEM_DISCONNECT_RESP_TYPE:
		  {
		    iWARPEM_Object_EndPoint_t* LocalEndPoint = gSockFdToEndPointMap[ SocketFd ];

		    //BegLogLine( FXLOG_IT_API_O_SOCKETS )
		    BegLogLine( FXLOG_IT_API_O_SOCKETS )
		      << "iWARPEM_DataReceiverThread(): iWARPEM_DISCONNECT_RESP_TYPE: "
		      << " LocalEndPoint: " << *LocalEndPoint
		      << " SocketFd: " << SocketFd
		      << EndLogLine;

		    StrongAssertLogLine( LocalEndPoint->ConnectedFlag == IWARPEM_CONNECTION_FLAG_ACTIVE_SIDE_PENDING_DISCONNECT )
		      << "iWARPEM_DataReceiverThread:: ERROR:: "
		      << " LocalEndPoint->ConnectedFlag: " << LocalEndPoint->ConnectedFlag
		      << EndLogLine;
		    
		    iwarpem_flush_queue( LocalEndPoint, IWARPEM_FLUSH_RECV_QUEUE_FLAG );
		    
		    struct epoll_event EP_Event;
		    EP_Event.events = EPOLLIN;
		    EP_Event.data.fd = SocketFd;

		    int mapepoll_ctl_rc = mapepoll_ctl( epoll_fd,
						  EPOLL_CTL_DEL, 
						  SocketFd,
						  & EP_Event );
		    
		    StrongAssertLogLine( mapepoll_ctl_rc == 0 )
		      << "iWARPEM_DataReceiverThread:: mapepoll_ctl() failed"
		      << " errno: " << errno
		      << EndLogLine;
		    
		    BegLogLine( FXLOG_IT_API_O_SOCKETS )
		      << "iWARPEM_DataReceiverThread:: Before close() "                      
		      << " SocketFd: " << SocketFd
		      << EndLogLine;		    

		    close( SocketFd );
		    
		    BegLogLine( FXLOG_IT_API_O_SOCKETS )
		      << "iWARPEM_DataReceiverThread:: After close() "
		      << " SocketFd: " << SocketFd
		      << EndLogLine;		    
		    
		    gSockFdToEndPointMap[ SocketFd ] = NULL;

		    LocalEndPoint->ConnectedFlag = IWARPEM_CONNECTION_FLAG_DISCONNECTED;
		    
		    /**********************************************
		     * Generate send completion event
		     *********************************************/
		    iWARPEM_Object_Event_t* CompletetionEvent = 
		      (iWARPEM_Object_Event_t*) malloc( sizeof( iWARPEM_Object_Event_t ) );
		    
		    it_connection_event_t* conne = (it_connection_event_t *) & CompletetionEvent->mEvent;  
		    		    
		    conne->event_number = IT_CM_MSG_CONN_DISCONNECT_EVENT;		    
		    conne->evd          = LocalEndPoint->connect_sevd_handle;
		    conne->ep           = (it_ep_handle_t) LocalEndPoint;		    
		    
		    iWARPEM_Object_EventQueue_t* RecvCmplEventQueue = 
		      (iWARPEM_Object_EventQueue_t*) conne->evd;
			
		    int enqrc = RecvCmplEventQueue->Enqueue( CompletetionEvent );
		    
		    StrongAssertLogLine( enqrc == 0 ) 
		      << "iWARPEM_DataReceiverThread()::Failed to enqueue connection request event" 
		      << EndLogLine;	  
		    /*********************************************/		    

		    break;
		  }
		case iWARPEM_DISCONNECT_REQ_TYPE:
		  {
		    // Clear send queue for the associated end point
		    iWARPEM_Object_EndPoint_t* LocalEndPoint = gSockFdToEndPointMap[ SocketFd ];
		    
		    // iwarpem_flush_queue( LocalEndPoint, IWARPEM_FLUSH_SEND_QUEUE_FLAG );
		    iwarpem_flush_queue( LocalEndPoint, IWARPEM_FLUSH_RECV_QUEUE_FLAG );

		    // BegLogLine( FXLOG_IT_API_O_SOCKETS )
		    BegLogLine( FXLOG_IT_API_O_SOCKETS )
		      << "iWARPEM_DataReceiverThread(): iWARPEM_DISCONNECT_REQ_TYPE: "
		      << " LocalEndPoint: " << *LocalEndPoint
		      << " SocketFd: " << SocketFd
		      << EndLogLine;

		    LocalEndPoint->ConnectedFlag = IWARPEM_CONNECTION_FLAG_PASSIVE_SIDE_PENDING_DISCONNECT;

		    // Wait for the active side to call close
		    iwarpem_it_ep_disconnect_resp( LocalEndPoint );
		    		    
		    break;
		  }
		case iWARPEM_DTO_SEND_TYPE:
		  {
		    StrongAssertLogLine( SocketFd >= 0 && 
					 SocketFd < SOCK_FD_TO_END_POINT_MAP_COUNT )
					   << "iWARPEM_DataReceiverThread:: "
					   << " SocketFd: "  << SocketFd
					   << " SOCK_FD_TO_END_POINT_MAP_COUNT: " << SOCK_FD_TO_END_POINT_MAP_COUNT
					   << EndLogLine;

		    iWARPEM_Object_EndPoint_t* LocalEndPoint = gSockFdToEndPointMap[ SocketFd ];

		    iWARPEM_Object_WorkRequest_t* RecvWR = NULL;

		    int DequeueStatus = LocalEndPoint->RecvWrQueue.Dequeue( & RecvWR );
		    
		    BegLogLine( FXLOG_IT_API_O_SOCKETS )
		      << "iWARPEM_DataReceiverThread(): Dequeued a recv request on: "
		      << " ep_handle: " << *LocalEndPoint
		      << " RecvWR: " << (void *) RecvWR
		      << " DequeueStatus: " << DequeueStatus
		      << EndLogLine;
		    
		    if( DequeueStatus != -1 )
		      {
			StrongAssertLogLine( RecvWR )
			  << "iWARPEM_DataReceiverThread(): ERROR:: RecvWR is NULL"
			  << EndLogLine;

			BegLogLine( FXLOG_IT_API_O_SOCKETS )
			  << "iWARPEM_DataReceiverThread:: in SEND_TYPE case: before loop over segments"
			  << " SocketFd: " << SocketFd
			  << " RecvWR->num_segments: " << RecvWR->num_segments
			  << EndLogLine;			    
			
			int BytesLeftToRead = Hdr.mTotalDataLen;

#if IT_API_CHECKSUM
                        uint64_t HdrChecksum = Hdr.mChecksum;
                        uint64_t NewChecksum = 0;
#endif

			int error = 0;
			for( int i = 0; 
			     ( i < RecvWR->num_segments ) && (BytesLeftToRead > 0); 
			     i++ )
			  {
			    iWARPEM_Object_MemoryRegion_t* MemRegPtr = 
			      (iWARPEM_Object_MemoryRegion_t *)RecvWR->segments_array[ i ].lmr;
			    
			    it_addr_mode_t AddrMode = MemRegPtr->addr_mode;
			    
			    char * DestAddr = 0;
			    
			    if( AddrMode == IT_ADDR_MODE_RELATIVE )
			      {
			      DestAddr = RecvWR->segments_array[ i ].addr.rel + (char *) MemRegPtr->addr;
			      BegLogLine(FXLOG_IT_API_O_SOCKETS)
			        << "DestAddr=" << RecvWR->segments_array[ i ].addr.rel
			        << "+" << (void *) MemRegPtr->addr
			        << " =" << (void *) DestAddr
			        << EndLogLine ;
			      }
			    else
			      {
			      DestAddr = (char *) RecvWR->segments_array[ i ].addr.abs;
			      }
			    
			    int length = ntohl(RecvWR->segments_array[ i ].length);
			    
			    BegLogLine( FXLOG_IT_API_O_SOCKETS )
			      << "iWARPEM_DataReceiverThread:: in SEND_TYPE case: before read_from_socket()"
			      << " SocketFd: " << SocketFd
			      << " i: " << i
			      << " AddrMode: " << AddrMode
			      << " Hdr.mTotalDataLen: " << Hdr.mTotalDataLen
			      << " DestAddr: " << (void *) DestAddr
			      << " length: " << length
			      << EndLogLine;			    

			    int ReadLength = min( length, BytesLeftToRead );
			    
			    int rlen;
			    iWARPEM_Status_t istatus = read_from_socket( SocketFd, 
									 DestAddr,
									 ReadLength,
									 & rlen );
			    if( istatus != IWARPEM_SUCCESS )
			      {
				struct epoll_event EP_Event;
				EP_Event.events = EPOLLIN;
				EP_Event.data.fd = SocketFd;
				
				int mapepoll_ctl_rc = mapepoll_ctl( epoll_fd,
							      EPOLL_CTL_DEL, 
							      SocketFd,
							      & EP_Event );
		  
				StrongAssertLogLine( mapepoll_ctl_rc == 0 )
				  << "iWARPEM_DataReceiverThread:: mapepoll_ctl() failed"
				  << " errno: " << errno
				  << EndLogLine;		    
				
				iwarpem_generate_conn_termination_event( SocketFd );
				error = 1;
				break;
			      }
			    
			    if( error )
			      continue;

#if IT_API_REPORT_BANDWIDTH_RECV
                            BandRecvStat.AddBytes( rlen );
#endif

#if IT_API_CHECKSUM
                            for( int j = 0; j < ReadLength; j++ )
                              NewChecksum += DestAddr[ j ];
#endif

			    BytesLeftToRead -= ReadLength;
			    
			    BegLogLine( FXLOG_IT_API_O_SOCKETS )
			      << "iWARPEM_DataReceiverThread:: in SEND_TYPE case: after read_from_socket()"
			      << " SocketFd: " << SocketFd
			      << " i: " << i
			      << " AddrMode: " << AddrMode
			      << " Hdr.mTotalDataLen: " << Hdr.mTotalDataLen
			      << " DestAddr: " << (void *) DestAddr
			      << " length: " << length
			      << EndLogLine;			    
			  } 

#if IT_API_CHECKSUM
                        StrongAssertLogLine( NewChecksum == HdrChecksum )
			  << "iWARPEM_DataReceiverThread:: in SEND_TYPE case: ERROR:: "  
                          << " NewChecksum: " << NewChecksum
                          << " HdrChecksum: " << HdrChecksum
                          << " SocketFd: " << SocketFd
                          << " Addr[ 0 ]:" << (void *) RecvWR->segments_array[ 0 ].addr.abs
                          << " Len[ 0 ]:" << RecvWR->segments_array[ 0 ].length
                          << EndLogLine;
#endif
			StrongAssertLogLine( BytesLeftToRead == 0 )
			  << "iWARPEM_DataReceiverThread:: in SEND_TYPE case: ERROR:: "  
			  << " Posted Receive does not provide enough space"
			  << EndLogLine;

			/**********************************************
			 * Generate send completion event
			 *********************************************/
			iWARPEM_Object_Event_t* DTOCompletetionEvent = 
			  (iWARPEM_Object_Event_t*) malloc( sizeof( iWARPEM_Object_Event_t ) );
			
			it_dto_cmpl_event_t* dtoce = (it_dto_cmpl_event_t*) & DTOCompletetionEvent->mEvent;  
			
			iWARPEM_Object_EndPoint_t* LocalEndPoint = (iWARPEM_Object_EndPoint_t*) RecvWR->ep_handle;
			
			dtoce->event_number = IT_DTO_RC_RECV_CMPL_EVENT;
			
			dtoce->evd          = LocalEndPoint->recv_sevd_handle; // i guess?
			dtoce->ep           = (it_ep_handle_t) RecvWR->ep_handle;
			dtoce->cookie       = RecvWR->cookie;
			dtoce->dto_status   = IT_DTO_SUCCESS;
			dtoce->transferred_length = RecvWR->mMessageHdr.mTotalDataLen;
			
			
			iWARPEM_Object_EventQueue_t* RecvCmplEventQueue = 
			  (iWARPEM_Object_EventQueue_t*) LocalEndPoint->recv_sevd_handle;
			
			BegLogLine( FXLOG_IT_API_O_SOCKETS )
			  << "iWARPEM_DataReceiverThread():: Enqueued recv cmpl event on: "  
                          << " RecvCmplEventQueue: " << (void *) RecvCmplEventQueue
                          << " DTOCompletetionEvent: " << (void *) DTOCompletetionEvent
			  << EndLogLine;

			int enqrc = RecvCmplEventQueue->Enqueue( DTOCompletetionEvent );
			
			StrongAssertLogLine( enqrc == 0 ) << "failed to enqueue connection request event" << EndLogLine;	  
			/*********************************************/

			BegLogLine( FXLOG_IT_API_O_SOCKETS )
			  << "About to call free( " << (void *) RecvWR->segments_array << " )"
			  << EndLogLine;
			
			free( RecvWR->segments_array );

			BegLogLine( FXLOG_IT_API_O_SOCKETS )
			  << "About to call free( " << (void *) RecvWR << " )"
			  << EndLogLine;

			free( RecvWR );
		      }
		    else 
		      {
			StrongAssertLogLine( 0 )
			  << "iWARPEM_DataReceiverThread:: ERROR:: Received a DTO_SEND_TYPE"
			  << " but no receive buffer has been posted" 
			  << EndLogLine;
		      }
    
		    break;
		  }
		case iWARPEM_DTO_RDMA_WRITE_TYPE:
		  {
#if IT_API_CHECKSUM
                    uint64_t HdrChecksum = Hdr.mChecksum;
                    uint64_t NewChecksum = 0;
#endif
        BegLogLine(FXLOG_IT_API_O_SOCKETS)
          << "Endian-converting from mRMRAddr=" << (void *) Hdr.mOpType.mRdmaWrite.mRMRAddr
          << " mRMRContext=" << (void *) Hdr.mOpType.mRdmaWrite.mRMRContext
          << EndLogLine
		    it_rdma_addr_t   RMRAddr    = be64toh(Hdr.mOpType.mRdmaWrite.mRMRAddr);
		    it_rmr_context_t RMRContext = be64toh(Hdr.mOpType.mRdmaWrite.mRMRContext);
//        it_rdma_addr_t   RMRAddr    = Hdr.mOpType.mRdmaWrite.mRMRAddr;
//        it_rmr_context_t RMRContext = Hdr.mOpType.mRdmaWrite.mRMRContext;
		    
        BegLogLine(FXLOG_IT_API_O_SOCKETS)
          << "RMRAddr=" << (void *) RMRAddr
          << " RMRContext=" << (void *) RMRContext
          << EndLogLine ;
		    iWARPEM_Object_MemoryRegion_t* MemRegPtr = 
		      (iWARPEM_Object_MemoryRegion_t *) RMRContext;
		    
		    it_addr_mode_t AddrMode = MemRegPtr->addr_mode;
		    
		    char * DestAddr = 0;

		    if( AddrMode == IT_ADDR_MODE_RELATIVE )		
		      DestAddr = RMRAddr + (char *) MemRegPtr->addr;
		    else
		      DestAddr = (char *) RMRAddr;	      		    

//		    Hdr.mTotalDataLen=ntohl(Hdr.mTotalDataLen) ;
		    BegLogLine( FXLOG_IT_API_O_SOCKETS )
		      << "iWARPEM_DataReceiverThread:: in RDMA_WRITE case: before read_from_socket()"
		      << " SocketFd: " << SocketFd
		      << " RMRAddr: " << (void *) RMRAddr
		      << " Hdr.mTotalDataLen: " << Hdr.mTotalDataLen
		      << " RMRContext: " << RMRContext
		      << EndLogLine;

		    int rlen;
		    iWARPEM_Status_t istatus = read_from_socket( SocketFd, 
								 DestAddr,
								 Hdr.mTotalDataLen,
								 &rlen );
		    
		    if( istatus != IWARPEM_SUCCESS )
		      {
			struct epoll_event EP_Event;
			EP_Event.events = EPOLLIN;
			EP_Event.data.fd = SocketFd;
				
			int mapepoll_ctl_rc = mapepoll_ctl( epoll_fd,
						      EPOLL_CTL_DEL, 
						      SocketFd,
						      & EP_Event );
		  
			StrongAssertLogLine( mapepoll_ctl_rc == 0 )
			  << "iWARPEM_DataReceiverThread:: mapepoll_ctl() failed"
			  << " errno: " << errno
			  << EndLogLine;		    
				
			iwarpem_generate_conn_termination_event( SocketFd );
			continue;
		      }

		    BegLogLine( FXLOG_IT_API_O_SOCKETS )
		      << "iWARPEM_DataReceiverThread:: in RDMA_WRITE case: after read_from_socket()"
		      << " SocketFd: " << SocketFd
		      << " RMRAddr: " << (void *) RMRAddr
		      << " Hdr.mTotalDataLen: " << Hdr.mTotalDataLen
		      << " RMRContext: " << RMRContext
		      << EndLogLine;

#if IT_API_REPORT_BANDWIDTH_RDMA_WRITE_IN
                    BandRdmaWriteInStat.AddBytes( rlen );
#endif

#if IT_API_CHECKSUM
                    for( int j = 0; j < Hdr.mTotalDataLen; j++ )
                      NewChecksum += DestAddr[ j ];

                    StrongAssertLogLine( NewChecksum == HdrChecksum )
                      << "iWARPEM_DataReceiverThread:: in RDMA_WRITE case: ERROR:: "  
                      << " NewChecksum: " << NewChecksum
                      << " HdrChecksum: " << HdrChecksum
                      << " SocketFd: " << SocketFd
                      << " Addr[ 0 ]:" << (void *) DestAddr
                      << " Len[ 0 ]:" << Hdr.mTotalDataLen
                      << EndLogLine;
#endif
		    
		    break;
		  }
		case iWARPEM_DTO_RDMA_READ_RESP_TYPE:
		  {
		    iWARPEM_Object_WorkRequest_t * LocalRdmaReadState = 
		      (iWARPEM_Object_WorkRequest_t * ) (Hdr.mOpType.mRdmaReadResp.mPrivatePtr);
		    
		    int TotalLeft = Hdr.mTotalDataLen;

                    LocalRdmaReadState->mMessageHdr.mTotalDataLen = TotalLeft;
                    
                    iWARPEM_Object_EndPoint_t* LocalEndPoint = (iWARPEM_Object_EndPoint_t*) LocalRdmaReadState->ep_handle;

#if IT_API_CHECKSUM
                    uint64_t HdrChecksum = Hdr.mChecksum;
                    uint64_t NewChecksum = 0;
#endif
		    
		    int error = 0;
		    for( int i = 0; i < LocalRdmaReadState->num_segments; i++ )
		      {
			it_lmr_triplet_t* LMRHdl = & LocalRdmaReadState->segments_array[ i ]; 

			iWARPEM_Object_MemoryRegion_t* MemRegPtr = 
			  (iWARPEM_Object_MemoryRegion_t *) LMRHdl->lmr;
		    
      BegLogLine(FXLOG_IT_API_O_SOCKETS)
			  << "LMRHdl->addr.abs=" << (void *) LMRHdl->addr.abs
			  << EndLogLine ;
			it_addr_mode_t AddrMode = MemRegPtr->addr_mode;
			
			char * DestAddr = 0;
			
			if( AddrMode == IT_ADDR_MODE_RELATIVE )		
			  DestAddr = LMRHdl->addr.rel + (char *) MemRegPtr->addr;
			else
			  DestAddr = (char *) LMRHdl->addr.abs;    
			
			int ToReadFromSocket = min( TotalLeft,
						    (int) LMRHdl->length );
			
			BegLogLine( FXLOG_IT_API_O_SOCKETS )
			  << "iWARPEM_DataReceiverThread:: in RDMA_READ_RESP case: before read_from_socket()"
			  << " SocketFd: " << SocketFd
			  << " DestAddr: " << (void *) DestAddr
			  << " ToReadFromSocket: " << ToReadFromSocket
			  << " TotalLeft: " << TotalLeft
			  << " LMRHdl->length: " << LMRHdl->length
			  << EndLogLine;

			int BytesRead;
			iWARPEM_Status_t istatus =  read_from_socket( SocketFd,
								      DestAddr,
								      ToReadFromSocket,
								      & BytesRead );
			
			if( istatus != IWARPEM_SUCCESS )
			  {
			    struct epoll_event EP_Event;
			    EP_Event.events = EPOLLIN;
			    EP_Event.data.fd = SocketFd;
			    
			    int mapepoll_ctl_rc = mapepoll_ctl( epoll_fd,
							  EPOLL_CTL_DEL, 
							  SocketFd,
							  & EP_Event );
			    
			    StrongAssertLogLine( mapepoll_ctl_rc == 0 )
			      << "iWARPEM_DataReceiverThread:: mapepoll_ctl() failed"
			      << " errno: " << errno
			      << EndLogLine;		    
			    
			    iwarpem_generate_conn_termination_event( SocketFd );
			    error = 1;
			    break;
			  }								
			
			BegLogLine( FXLOG_IT_API_O_SOCKETS )
			  << "iWARPEM_DataReceiverThread:: in RDMA_READ_RESP case: after read_from_socket()"
                          << " LocalEndPoint: " << *LocalEndPoint
			  << " SocketFd: " << SocketFd
			  << " DestAddr: " << (void *) DestAddr
			  << " ToReadFromSocket: " << ToReadFromSocket
			  << " TotalLeft: " << TotalLeft
			  << " LMRHdl->length: " << LMRHdl->length
			  << EndLogLine;

			TotalLeft -= BytesRead;	

#if IT_API_REPORT_BANDWIDTH_RDMA_READ_IN
                        BandRdmaReadInStat.AddBytes( BytesRead );
#endif
                        
#if IT_API_CHECKSUM
                        for( int j = 0; j < ToReadFromSocket; j++ )
                          NewChecksum += DestAddr[ j ];
#endif

		      }

		    if( error )
		      continue;

#if IT_API_CHECKSUM
                    StrongAssertLogLine( NewChecksum == HdrChecksum )
                      << "iWARPEM_DataReceiverThread:: in RDMA_READ_RESP case: ERROR:: "  
                      << " NewChecksum: " << NewChecksum
                      << " HdrChecksum: " << HdrChecksum
                      << " SocketFd: " << SocketFd
                      << " Addr[ 0 ]: " << (void *) LocalRdmaReadState->segments_array[ 0 ].addr.abs
                      << " Len[ 0 ]: " << LocalRdmaReadState->segments_array[ 0 ].length
                      << EndLogLine;
#endif
		    AssertLogLine( TotalLeft == 0 )
		      << "iWARPEM_DataReceiverThread:: in RDMA_READ_RESP case: "
		      << " TotalLeft: " << TotalLeft
		      << EndLogLine;

                    iwarpem_generate_rdma_read_cmpl_event( LocalRdmaReadState );

		    break;
		  }
		case iWARPEM_DTO_RDMA_READ_REQ_TYPE:
		  {
		    // Post Rdma Write
		    it_rdma_addr_t   RMRAddr    = Hdr.mOpType.mRdmaReadReq.mRMRAddr;
		    it_rmr_context_t RMRContext = Hdr.mOpType.mRdmaReadReq.mRMRContext;
		    int              ReadLen    = Hdr.mOpType.mRdmaReadReq.mDataToReadLen;
		    
		    iWARPEM_Object_WorkRequest_t * RdmaReadClientWorkRequestState =
		     (iWARPEM_Object_WorkRequest_t *) Hdr.mOpType.mRdmaReadReq.mPrivatePtr;
		    
		    BegLogLine( FXLOG_IT_API_O_SOCKETS )
		      << "iWARPEM_DataReceiverThread(): case iWARPEM_DTO_RDMA_READ_REQ_TYPE "
		      << " RMRAddr: " << (void *) RMRAddr
		      << " RMRContext: " << (void *) RMRContext
		      << " ReadLen: " << ReadLen
		      << EndLogLine;

		    it_lmr_triplet_t LocalSegment;
		    LocalSegment.lmr    = (it_lmr_handle_t) RMRContext;
		    LocalSegment.length =   ReadLen;		   
		    LocalSegment.addr.abs = (void *) RMRAddr;
		    
		    iwarpem_it_post_rdma_read_resp( SocketFd,
						    & LocalSegment,
						    RdmaReadClientWorkRequestState );
		    
		    break;
		  }
		default:
		  {
		    StrongAssertLogLine( 0 )
		      << "iWARPEM_DataReceiverThread:: ERROR:: case not recognized: "
		      << " Hdr.mMsg_Type: " << (void *) Hdr.mMsg_Type
		      << EndLogLine;
		  }
		}				
	    }
	}
    }
      
   return NULL;
}

void
iWARPEM_ProcessSendWR( iWARPEM_Object_WorkRequest_t* SendWR )
{
  /**********************************************
   * Send Header to destination
   *********************************************/
  int SocketFD = ((iWARPEM_Object_EndPoint_t *) SendWR->ep_handle)->ConnFd;
	  
  int LenToSend = sizeof( iWARPEM_Message_Hdr_t );

  SendWR->mMessageHdr.EndianConvert() ;
  BegLogLine( FXLOG_IT_API_O_SOCKETS )
    << "iWARPEM_ProcessSendWR(): "
    << " SendWR->ep_handle: " << *((iWARPEM_Object_EndPoint_t *)SendWR->ep_handle)
    << " SocketFD: " << SocketFD
    << " SendWR: " << (void *) SendWR
    << " SendWR->mMessageHdr.mMsg_Type: " << SendWR->mMessageHdr.mMsg_Type
    << " &SendWR->mMessageHdr.mMsg_Type: " << (void *) &(SendWR->mMessageHdr.mMsg_Type)
    << " SendWR->num_segments: " << SendWR->num_segments
    << " SendWR->dto_flags: " << SendWR->dto_flags
    << " SendWR->dto_flags & IT_COMPLETION_FLAG: " << (SendWR->dto_flags & IT_COMPLETION_FLAG)
    << " SendWR->dto_flags & IT_NOTIFY_FLAG: " << (SendWR->dto_flags & IT_NOTIFY_FLAG)
    << EndLogLine;

  iWARPEM_Msg_Type_t MsgType = SendWR->mMessageHdr.mMsg_Type;

  if( MsgType == iWARPEM_DTO_RDMA_READ_CMPL_TYPE )
    {
      /**********************************************
       * Generate rdma completion event
       *********************************************/
      if( ( SendWR->dto_flags & IT_COMPLETION_FLAG ) && 
          ( SendWR->dto_flags & IT_NOTIFY_FLAG ) )
        {
          iWARPEM_Object_Event_t DTOCompletetionEvent ;
		    
          it_dto_cmpl_event_t* dtoce = (it_dto_cmpl_event_t*) & DTOCompletetionEvent.mEvent;
		    
          dtoce->event_number       = IT_DTO_RDMA_READ_CMPL_EVENT;
          dtoce->evd                = ((iWARPEM_Object_EndPoint_t *)SendWR->ep_handle)->request_sevd_handle;
          dtoce->ep                 = (it_ep_handle_t) SendWR->ep_handle;
          dtoce->cookie             = SendWR->cookie;
          dtoce->dto_status         = IT_DTO_SUCCESS;
          dtoce->transferred_length = ntohl(SendWR->mMessageHdr.mTotalDataLen);
		    
          iWARPEM_Object_EventQueue_t* SendCmplEventQueue =
            (iWARPEM_Object_EventQueue_t*) dtoce->evd;

//          BegLogLine(FXLOG_IT_API_O_SOCKETS)
//            << "RDMA read completion, queue=" << SendCmplEventQueue
//            << " transferred_length=" << dtoce->transferred_length
//            << EndLogLine ;
//          int enqrc = SendCmplEventQueue->Enqueue( DTOCompletetionEvent );
//          BegLogLine(FXLOG_IT_API_O_SOCKETS)
//            << "Posted completion, enqrc=" << enqrc
//            << EndLogLine ;
          BegLogLine(FXLOG_IT_API_O_SOCKETS)
            << "RDMA read completion, wants queue=" << gSendCmplQueue
            << EndLogLine ;
          int enqrc=gSendCmplQueue->Enqueue(*(it_event_t *) dtoce) ;
          it_api_o_sockets_signal_accept() ;
		    
          StrongAssertLogLine( enqrc == 0 ) 
            << "iWARPEM_DataReceiverThread(): failed to enqueue connection request event" 
            << EndLogLine;	  
          /*********************************************/
        }
		
      BegLogLine( FXLOG_IT_API_O_SOCKETS )
        << "iWARPEM_DataReceiverThread(): About to call free( " 
        << (void *) SendWR->segments_array << " )"
        << EndLogLine;
		
      free( SendWR->segments_array );
		
      BegLogLine( FXLOG_IT_API_O_SOCKETS )
        << "iWARPEM_DataReceiverThread(): About to call free( " << (void *) SendWR << " )"
        << EndLogLine;
		
      free( SendWR );
    }
  else 
    {
      int wlen = 0;
      SendWR->mMessageHdr.EndianConvert() ;
      iWARPEM_Status_t istatus = write_to_socket( SocketFD, 
                                                  (char *) & SendWR->mMessageHdr, 
                                                  LenToSend,
                                                  & wlen );
      if( istatus != IWARPEM_SUCCESS )
        {
          iwarpem_generate_conn_termination_event( SocketFD );
          return;
        }          

      AssertLogLine( wlen == LenToSend )
        << "iWARPEM_ProcessSendWR(): ERROR: "
        << " LenToSend: " << LenToSend
        << " wlen: " << wlen
        << EndLogLine;

      switch( MsgType )
        {
        case iWARPEM_DISCONNECT_REQ_TYPE:
          {
            BegLogLine( FXLOG_IT_API_O_SOCKETS )
              << "iWARPEM_ProcessSendWR(): About to call free(): " 
              << " MsgType: " << iWARPEM_Msg_Type_to_string( MsgType )
              << " SendWR->ep_handle: " << *((iWARPEM_Object_EndPoint_t *)SendWR->ep_handle)
              << " SendWR: " << (void *) SendWR
              << EndLogLine;

            free( SendWR );

            break;
          }
        case iWARPEM_DISCONNECT_RESP_TYPE:
          {		
            BegLogLine( FXLOG_IT_API_O_SOCKETS )
              << "iWARPEM_ProcessSendWR(): About to call free(): " 
              << " MsgType: " << iWARPEM_Msg_Type_to_string( MsgType )
              << " SendWR->ep_handle: " << *((iWARPEM_Object_EndPoint_t *) SendWR->ep_handle)
              << " SendWR: " << (void *) SendWR
              << EndLogLine;

            iwarpem_flush_queue( (iWARPEM_Object_EndPoint_t *) SendWR->ep_handle, IWARPEM_FLUSH_SEND_QUEUE_FLAG );

            free( SendWR );
                
            break;
          }
        case iWARPEM_DTO_RDMA_READ_REQ_TYPE:
          {
            // Freeing of the SendWR happens in the ReceiverThread when the RDMA_READ_RESP is processed.
            // WARNING: WARNING: WARNING: 
            // WARNING: WARNING: WARNING: 
            // WARNING: WARNING: WARNING: 
            // WARNING: WARNING: WARNING: 
            // WARNING: WARNING: WARNING: 
                
            // Due to thread scheduling it is possible for the RDMA_READ_RESP 
            // to be processed on this node before the switch statement is entered                
            // In which case SendWR has already been freed. It's not valid to use it here 
            // without appropriate locking                

            break;
          }
        case iWARPEM_DTO_RDMA_READ_RESP_TYPE:
        case iWARPEM_DTO_RDMA_WRITE_TYPE:
        case iWARPEM_DTO_SEND_TYPE:
          {
            /**********************************************
             * Send data to destination
             *********************************************/		

#if IT_API_CHECKSUM
            uint64_t Checksum = 0;
#endif

            int error = 0;
            for( int i = 0; i < SendWR->num_segments; i++ )
              {
                iWARPEM_Object_MemoryRegion_t* MemRegPtr = (iWARPEM_Object_MemoryRegion_t *)SendWR->segments_array[ i ].lmr;
	      
                BegLogLine(FXLOG_IT_API_O_SOCKETS)
                  << "SendWR->segments_array[" << i
                  << "]=" << HexDump(SendWR->segments_array+i,sizeof(SendWR->segments_array[ i ]))
                  << EndLogLine ;
                AssertLogLine( MemRegPtr != NULL )
                  << "iWARPEM_ProcessSendWR(): ERROR: "
                  << " SendWR->ep_handle: " << *((iWARPEM_Object_EndPoint_t *)SendWR->ep_handle)
                  << " MsgType: "
                  << MsgType
                  << EndLogLine;

                BegLogLine(FXLOG_IT_API_O_SOCKETS)
                  << "MemRegPtr=" << (void *) MemRegPtr
                  << " SendWR->segments_array[ i ].addr.abs=" << (void *) SendWR->segments_array[ i ].addr.abs
                  << EndLogLine ;

                it_addr_mode_t AddrMode = MemRegPtr->addr_mode;
	      
                char * DestAddr = NULL;

                if( AddrMode == IT_ADDR_MODE_RELATIVE )		
                  DestAddr = SendWR->segments_array[ i ].addr.rel + (char *)MemRegPtr->addr;
                else
                  DestAddr = (char *) SendWR->segments_array[ i ].addr.abs;
	      
                BegLogLine( FXLOG_IT_API_O_SOCKETS )
                  << "iWARPEM_ProcessSendWR(): "
                  << " SocketFD: " << SocketFD
                  << " SendWR->ep_handle: " << *((iWARPEM_Object_EndPoint_t *)SendWR->ep_handle)
                  << " DestAddr: " << (void *) DestAddr
                  << " SendWR->segments_array[ " << i << " ].length: " << SendWR->segments_array[ i ].length
                  << " AddrMode: " << AddrMode
                  << EndLogLine;
		    
#if IT_API_CHECKSUM
                for( int j = 0; j < SendWR->segments_array[ i ].length; j++ )
                  Checksum += DestAddr[ j ];
#endif
                int wlen;
                iWARPEM_Status_t istatus = write_to_socket( SocketFD,
                                                            DestAddr,
                                                            ntohl(SendWR->segments_array[ i ].length),
                                                            & wlen );
                if( istatus != IWARPEM_SUCCESS )
                  {
                    iwarpem_generate_conn_termination_event( SocketFD );
                    error = 1;
                    break;
                  } 
              }
		
            if( error )
              {
                goto free_WR_and_break;
              }

#if IT_API_CHECKSUM
            StrongAssertLogLine( Checksum == SendWR->mMessageHdr.mChecksum )
              << "iWARPEM_ProcessSendWR(): ERROR: "
              << " Checksum: " << Checksum
              << " SendWR->mMessageHdr.mChecksum: " << SendWR->mMessageHdr.mChecksum
              << " SendWR: " << (void *) SendWR
              << " Addr[ 0 ]: " << (void *) SendWR->segments_array[ 0 ].addr.abs
              << " Len[ 0 ]: " << SendWR->segments_array[ 0 ].length
              << EndLogLine;
#endif

            /**********************************************/
		

            /**********************************************
             * Generate send completion event
             *********************************************/
            if( ( SendWR->dto_flags & IT_COMPLETION_FLAG ) && 
                ( SendWR->dto_flags & IT_NOTIFY_FLAG ) )
              {
                SendWR->mMessageHdr.EndianConvert() ;
                iWARPEM_Object_Event_t DTOCompletetionEvent ;
		    
                it_dto_cmpl_event_t* dtoce = (it_dto_cmpl_event_t*) & DTOCompletetionEvent.mEvent;
		    
                iWARPEM_Object_EndPoint_t* LocalEndPoint = (iWARPEM_Object_EndPoint_t*) SendWR->ep_handle;
		    
                if( SendWR->mMessageHdr.mMsg_Type == iWARPEM_DTO_SEND_TYPE )
                  dtoce->event_number = IT_DTO_SEND_CMPL_EVENT;
                else if( SendWR->mMessageHdr.mMsg_Type == iWARPEM_DTO_RDMA_WRITE_TYPE )
                  dtoce->event_number = IT_DTO_RDMA_WRITE_CMPL_EVENT;
                else 
                  StrongAssertLogLine( 0 ) 
                    << "iWARPEM_ProcessSendWR:: ERROR:: "
                    << " SendWR->mMessageHdr.mMsg_Type: " << SendWR->mMessageHdr.mMsg_Type
                    << EndLogLine;
		    
                dtoce->evd          = LocalEndPoint->request_sevd_handle; // i guess?
                dtoce->ep           = (it_ep_handle_t) SendWR->ep_handle;
                dtoce->cookie       = SendWR->cookie;
                dtoce->dto_status   = IT_DTO_SUCCESS;
                dtoce->transferred_length = ntohl(SendWR->mMessageHdr.mTotalDataLen);
		    		    
                int* CookieAsIntPtr = (int *) & dtoce->cookie;

                BegLogLine( 0 )
                  << "iWARPEM_ProcessSendWR(): "
                  << " SendWR: " << (void *) SendWR
                  << " SendWR->ep_handle: " << *((iWARPEM_Object_EndPoint_t *)SendWR->ep_handle)
                  << " DTO_Type: " << SendWR->mMessageHdr.mMsg_Type
                  << " dtoce->evd: " << (void *) dtoce->evd
                  << " dtoce->ep: " << (void *) dtoce->ep
                  << " dtoce->cookie: " 
                  << FormatString( "%08X" ) << CookieAsIntPtr[ 0 ] 
                  << " "
                  << FormatString( "%08X" ) << CookieAsIntPtr[ 1 ] 
                  << " dtoce->transferred_length: " << dtoce->transferred_length
                  << EndLogLine;

                iWARPEM_Object_EventQueue_t* SendCmplEventQueue =
                  (iWARPEM_Object_EventQueue_t*) LocalEndPoint->request_sevd_handle;
//
//                BegLogLine(FXLOG_IT_API_O_SOCKETS)
//                  << "RDMA write completion, queue=" << SendCmplEventQueue
//                  << " transferred_length=" << dtoce->transferred_length
//                  << EndLogLine ;
//                int enqrc = SendCmplEventQueue->Enqueue( DTOCompletetionEvent );
                BegLogLine(FXLOG_IT_API_O_SOCKETS)
                  << "RDMA write completion, wants queue=" << gSendCmplQueue
                  << EndLogLine ;
                int enqrc=gSendCmplQueue->Enqueue(*(it_event_t *) dtoce) ;
                it_api_o_sockets_signal_accept() ;
		    
                StrongAssertLogLine( enqrc == 0 ) << "failed to enqueue connection request event" << EndLogLine;	  
                /*********************************************/
              }
		
          free_WR_and_break:
            if( SendWR->segments_array != NULL )
              {
                BegLogLine( FXLOG_IT_API_O_SOCKETS )
                  << "iWARPEM_ProcessSendWR(): "
                  << " About to call free( " << (void *) SendWR->segments_array << " )"
                  << EndLogLine;
                    
                free( SendWR->segments_array );
                SendWR->segments_array = NULL;
              }
		
            BegLogLine( FXLOG_IT_API_O_SOCKETS )
              << "iWARPEM_ProcessSendWR(): " 
              << "About to call free( " << (void *) SendWR << " )"
              << EndLogLine;
		
            free( SendWR );
		
            break;
          }
        default:
          {
            StrongAssertLogLine( 0 )
              << "iWARPEM_ProcessSendWR(): ERROR:: Type not recognized "
              << " SendWR->mMessageHdr.mMsg_Type: " << SendWR->mMessageHdr.mMsg_Type
              << EndLogLine;
          }
        }
    }
}

void*
iWARPEM_DataSenderThread( void* args )
{
  BegLogLine( FXLOG_IT_API_O_SOCKETS )
    << "iWARPEM_DataSenderThread(): Started!"
    << EndLogLine;

  pthread_mutex_unlock( & gSendThreadStartedMutex );

  int Even=1;
  BegLogLine(FXLOG_IT_API_O_SOCKETS)
    << "gRecvToSendWrQueue=" << gRecvToSendWrQueue
    << " gSendWrQueue=" << gSendWrQueue
    << EndLogLine ;
  // Process the sender WR queue
  while( 1 ) 
    {
      iWARPEM_Object_WorkRequest_t* SendWR = NULL;
      int dstat_0 = -1;
      int dstat_1 = -1;
      
      if( Even )
        {
          dstat_0 = gRecvToSendWrQueue->mQueue.Dequeue( &SendWR );
          
          if( ( dstat_0 != -1 ) && ( SendWR != NULL ) )
            iWARPEM_ProcessSendWR( SendWR );
          
          dstat_1 = gSendWrQueue->mQueue.Dequeue( &SendWR );
          
          if( ( dstat_1 != -1 ) && ( SendWR != NULL ) )
            iWARPEM_ProcessSendWR( SendWR );
          
        }
      else 
        {
          dstat_1 = gSendWrQueue->mQueue.Dequeue( &SendWR );
          
          if( ( dstat_1 != -1 ) && ( SendWR != NULL ) )
            iWARPEM_ProcessSendWR( SendWR );
          
          dstat_0 = gRecvToSendWrQueue->mQueue.Dequeue( &SendWR );
          
          if( ( dstat_0 != -1 ) && ( SendWR != NULL ) )
            iWARPEM_ProcessSendWR( SendWR );

        }
      
      if( ( dstat_0 == -1 ) && (dstat_0 == -1) )
        {
          
          // This is a dummy mutex needed as an argument to pthread_cond_timedwait
          pthread_mutex_lock( &gBlockMutex );
          
          struct timespec CondWaitTimeout;
          CondWaitTimeout.tv_sec = 0;
          CondWaitTimeout.tv_nsec = 10000000; //0.01

          gBlockedFlag = 1;
#if defined(__powerpc__)
          _bgp_msync();
#endif

          BegLogLine( 0 )
            << "iWARPEM_DataSenderThread(): Queues are empty about to pthread_cond_timedwait() "
            << " gBlockedFlag: " << gBlockedFlag
            << EndLogLine;
          struct timespec now ;
          clock_gettime(CLOCK_REALTIME,&now) ;
          struct timespec expiry ;
          long nsec=now.tv_nsec + CondWaitTimeout.tv_nsec ;
          if (nsec >= 1000000000)
            {
              expiry.tv_nsec=nsec-1000000000 ;
              expiry.tv_sec=now.tv_sec+CondWaitTimeout.tv_sec+1 ;
            }
          else
            {
              expiry.tv_nsec=nsec ;
              expiry.tv_sec=now.tv_sec+CondWaitTimeout.tv_sec ;
            }
          
          pthread_cond_timedwait( &gBlockCond, &gBlockMutex, &expiry );
          
          pthread_mutex_unlock( &gBlockMutex );
          
          gBlockedFlag = 0;
#if defined(__powerpc__)
          _bgp_msync();
#endif

          // BegLogLine( FXLOG_IT_API_O_SOCKETS )
          BegLogLine( 0 )
            << "iWARPEM_DataSenderThread(): After pthread_cond_timedwait() "
            << " gBlockedFlag: " << gBlockedFlag
            << EndLogLine;
        }
            

      Even = ! Even;
    }

  return NULL;
}

pthread_mutex_t         gDataReceiverControlSockMutex;
int gDataReceiverControlSockFd = -1;

void
iwarpem_add_socket_to_list( int aSockFd,
			    iWARPEM_Object_EndPoint_t* aEP )
{
    BegLogLine(FXLOG_IT_API_O_SOCKETS_CONNECT)
        << "aSockFd=" << aSockFd
        << " aEP=" << aEP
        << EndLogLine ;
  pthread_mutex_lock( & gDataReceiverControlSockMutex );

  StrongAssertLogLine( aSockFd >= 0 && 
		       aSockFd < SOCK_FD_TO_END_POINT_MAP_COUNT )
			 << "iwarpem_add_socket_to_list():: ERROR:: "
			 << " aSockFd: "  << aSockFd
			 << " SOCK_FD_TO_END_POINT_MAP_COUNT: " << SOCK_FD_TO_END_POINT_MAP_COUNT
			 << EndLogLine;

  StrongAssertLogLine( gSockFdToEndPointMap[ aSockFd ] == NULL )
    << "iwarpem_add_socket_to_list():: ERROR:: "
    << " aSockFd: " << aSockFd
    << EndLogLine;

  gSockFdToEndPointMap[ aSockFd ] = aEP;

  iWARPEM_SocketControl_Hdr_t Hdr;
  Hdr.mOpType = IWARPEM_SOCKETCONTROL_TYPE_ADD;
  Hdr.mSockFd = aSockFd;
  
  int wlen;
  write_to_socket( gDataReceiverControlSockFd, 
		   (char *) & Hdr,
		   sizeof( iWARPEM_SocketControl_Hdr_t ),
		   & wlen );

  pthread_mutex_unlock( & gDataReceiverControlSockMutex );
}

void
iwarpem_remove_socket_to_list( int aSockFd )
{
    BegLogLine(FXLOG_IT_API_O_SOCKETS_CONNECT)
        << "aSockFd=" << aSockFd
        << EndLogLine ;
  pthread_mutex_lock( & gDataReceiverControlSockMutex );

  StrongAssertLogLine( aSockFd >= 0 && 
		       aSockFd < SOCK_FD_TO_END_POINT_MAP_COUNT )
			 << "iwarpem_add_socket_to_list():: ERROR:: "
			 << " aSockFd: "  << aSockFd
			 << " SOCK_FD_TO_END_POINT_MAP_COUNT: " << SOCK_FD_TO_END_POINT_MAP_COUNT
			 << EndLogLine;

  StrongAssertLogLine( gSockFdToEndPointMap[ aSockFd ] == NULL )
    << "iwarpem_add_socket_to_list():: ERROR:: "
    << " aSockFd: " << aSockFd
    << EndLogLine;

  iWARPEM_SocketControl_Hdr_t Hdr;
  
  Hdr.mOpType = IWARPEM_SOCKETCONTROL_TYPE_REMOVE;
  Hdr.mSockFd = aSockFd;
  
  int wlen;
  write_to_socket( gDataReceiverControlSockFd, 
		   (char *) & Hdr,
		   sizeof( iWARPEM_SocketControl_Hdr_t ),
		   &wlen );

  pthread_mutex_unlock( & gDataReceiverControlSockMutex );
}


it_status_t it_ia_create (
  IN  const char           *name,
  IN        uint32_t        major_version,
  IN        uint32_t        minor_version,
  OUT       it_ia_handle_t *ia_handle
  )
{
  pthread_mutex_lock( & gITAPI_INITMutex );

  if( gITAPI_Initialized == 0 )
    {
  
  BegLogLine(FXLOG_IT_API_O_SOCKETS)
    << "it_ia_create(): Entering about to call   gSendWRLocalEndPointList.Init();"
    << " IN name "          << name
    << " IN major_version " << major_version
    << " IN minor_version " << minor_version
    << " OUT ia_handle "    << (void *) *ia_handle
    << EndLogLine;

  gSendWrQueue = (iWARPEM_Object_WR_Queue_t *) malloc( sizeof( iWARPEM_Object_WR_Queue_t ) );
  gSendWrQueue->Init( IWARPEM_SEND_WR_QUEUE_MAX_SIZE );

  gRecvToSendWrQueue = (iWARPEM_Object_WR_Queue_t *) malloc( sizeof( iWARPEM_Object_WR_Queue_t ) );
  gRecvToSendWrQueue->Init( IWARPEM_SEND_WR_QUEUE_MAX_SIZE );
  pthread_cond_init( & gBlockCond, NULL );
  pthread_mutex_init( & gBlockMutex, NULL );
  gBlockedFlag = 0;

#if IT_API_REPORT_BANDWIDTH_OUTGOING_TOTAL
  gBandOutStat.Init( "Outgoing", IT_API_REPORT_BANDWIDTH_OUTGOING_MODULO_BYTES );

  BegLogLine( FXLOG_IT_API_O_SOCKETS )
    << "it_ia_create(): "
    << " After Init on gBandOutStat."
    << EndLogLine;
#endif

#if IT_API_REPORT_BANDWIDTH_RDMA_WRITE_IN
  iWARPEM_Bandwidth_Stats_t BandRdmaWriteInStat;
  BandRdmaWriteInStat.Init( "RdmaWriteIn" );
#endif

#if IT_API_REPORT_BANDWIDTH_RDMA_READ_IN
  iWARPEM_Bandwidth_Stats_t BandRdmaReadInStat;
  BandRdmaReadInStat.Init( "RdmaReadIn" );
#endif

#if IT_API_REPORT_BANDWIDTH_RECV
  iWARPEM_Bandwidth_Stats_t BandRecvStat;
  BandRecvStat.Init( "Recv" );
#endif

#if IT_API_REPORT_BANDWIDTH_INCOMMING_TOTAL
  gBandInStat.Init( "Incomming" );
#endif    
  
  pthread_mutex_init( & gGenerateConnTerminationEventMutex, NULL ); 

  gReadCountHistogram.Init( "ReadIterCount", 2,  10, 8 );
  gReadTimeHistogram.Init( "ReadTime", 0,  100000, 128 );
  
  /*************************************************
   * Set up the Data Receiver Control Socket
   *************************************************/


  /***** Set up the server end *****/
  struct sockaddr    * drc_serv_saddr;
  int                  drc_serv_socket;

#ifdef IT_API_OVER_UNIX_DOMAIN_SOCKETS
  struct sockaddr_un   drc_serv_addr;
  bzero( (char *) &drc_serv_addr, sizeof( drc_serv_addr ) );
  drc_serv_addr.sun_family      = IT_API_SOCKET_FAMILY;
  
  sprintf( drc_serv_addr.sun_path, 
           "%s.%d",
           IT_API_UNIX_SOCKET_DRC_PATH,
           getpid() );

  unlink( drc_serv_addr.sun_path );
  
  int drc_serv_addr_len       = sizeof( drc_serv_addr.sun_family ) + strlen( drc_serv_addr.sun_path );
#else
  struct sockaddr_in   drc_serv_addr;
  bzero( (char *) &drc_serv_addr, sizeof( drc_serv_addr ) );
  drc_serv_addr.sin_family      = IT_API_SOCKET_FAMILY;
  drc_serv_addr.sin_port        = htons( 0 );
  drc_serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);

  int drc_serv_addr_len         = sizeof( drc_serv_addr );
#endif


  drc_serv_saddr = (struct sockaddr *)& drc_serv_addr;

  if((drc_serv_socket = socket(IT_API_SOCKET_FAMILY, SOCK_STREAM, 0)) < 0)
    {
    perror("Data Receiver Control socket() open");
    StrongAssertLogLine( 0 ) << EndLogLine;
    }

  StrongAssertLogLine( drc_serv_socket >= 0 )
    << "it_ia_create(): "
    << " Failed to create Data Receiver Control socket "
    << " Errno " << errno
    << EndLogLine;

#ifndef IT_API_OVER_UNIX_DOMAIN_SOCKETS
  int True = 1;
  setsockopt( drc_serv_socket, SOL_SOCKET, SO_REUSEADDR, (char *)&True, sizeof( True ) );
#endif

  int brc;
  if( brc = bind( drc_serv_socket, drc_serv_saddr, drc_serv_addr_len ) < 0)
    {
    perror("Data Receiver Control socket bind()");
    close( drc_serv_socket );
    StrongAssertLogLine( 0 ) << EndLogLine;
    }

  StrongAssertLogLine( brc >= 0 )
    << "it_ia_create(): "
    << " Failed to bind Data Receiver Control socket "
    << " Errno " << errno
    << EndLogLine;  
  /*********************************/



  /***** Set up the client end *****/  
#ifndef IT_API_OVER_UNIX_DOMAIN_SOCKETS
  // Get the server port to connect on  
  int gsnrc;
  socklen_t drc_serv_addrlen = sizeof( drc_serv_addr );
  if( gsnrc = getsockname(drc_serv_socket, drc_serv_saddr, &drc_serv_addrlen) != 0)
    {
    perror("getsockname()");    
    close( drc_serv_socket );

    StrongAssertLogLine( 0 ) << EndLogLine;
    }

  int drc_serv_port = drc_serv_addr.sin_port;  

  BegLogLine(FXLOG_IT_API_O_SOCKETS)
    << "it_ia_create(): after getsockname(): "
    << " drc_serv_port: " << drc_serv_port
    << EndLogLine;  
  
  struct sockaddr_in   drc_cli_addr;
  int                  drc_cli_socket;

  bzero( (void *) & drc_cli_addr, sizeof( struct sockaddr_in ) );
  drc_cli_addr.sin_family      = IT_API_SOCKET_FAMILY;
  drc_cli_addr.sin_port        = drc_serv_port;
  drc_cli_addr.sin_addr.s_addr = *(unsigned int *)(gethostbyname( "localhost" )->h_addr);
//  drc_cli_addr.sin_addr.s_addr = *(unsigned long *)(gethostbyname( "127.0.0.1" )->h_addr);
//  drc_cli_addr.sin_addr.s_addr = 0x7f000001;

  BegLogLine(FXLOG_IT_API_O_SOCKETS)
    << "it_ia_create():  "  
    << " drc_cli_addr.sin_family: " << drc_cli_addr.sin_family
    << " drc_cli_addr.sin_port: " << drc_cli_addr.sin_port
    << " drc_cli_addr.sin_addr.s_addr: " << HexDump(&drc_cli_addr.sin_addr.s_addr,sizeof(drc_cli_addr.sin_addr.s_addr))
    << " drc_cli_addr.sin_addr.s_addr: " << drc_cli_addr.sin_addr.s_addr
    << EndLogLine;  

  int drc_cli_addr_len       = sizeof( drc_cli_addr );
#else

  struct sockaddr_un   drc_cli_addr;
  int                  drc_cli_socket;

  bzero( (void *) & drc_cli_addr, sizeof( drc_cli_addr ) );
  drc_cli_addr.sun_family      = IT_API_SOCKET_FAMILY;
  
  sprintf( drc_cli_addr.sun_path, 
           "%s.%d",
           IT_API_UNIX_SOCKET_DRC_PATH,
           getpid() );

  int drc_cli_addr_len       = sizeof( drc_cli_addr.sun_family ) + strlen( drc_cli_addr.sun_path );
#endif

  if((drc_cli_socket = socket(IT_API_SOCKET_FAMILY, SOCK_STREAM, 0)) < 0)
    {
    perror("Data Receiver Control socket() open");
    StrongAssertLogLine( 0 ) << EndLogLine;
    }

  StrongAssertLogLine( drc_cli_socket >= 0 )
    << "it_ia_create(): ERROR: "
    << " Failed to create Data Receiver Control socket "
    << " Errno " << errno
    << EndLogLine;
    
  /*************************************************/
  

  if( listen( drc_serv_socket, 5 ) < 0 )
    {
      perror( "listen failed" );

      StrongAssertLogLine( 0 ) << EndLogLine;
      
      exit( -1 );
    }
  BegLogLine(FXLOG_IT_API_O_SOCKETS)
    << "Listening"
    << EndLogLine ;

  /***********************************************
   * Start the data receive thread
   ***********************************************/
  pthread_t DataReceiverTID;

  gDataReceiverThreadArgs.drc_cli_socket = drc_cli_socket;
  memcpy( & gDataReceiverThreadArgs.drc_cli_addr,
	  & drc_cli_addr,
	   drc_cli_addr_len );
  
  BegLogLine(FXLOG_IT_API_O_SOCKETS)
    << "iWARPEM_DataReceiverThread:: Before connect()"
    << " drc_client_socket: " << drc_cli_socket
    << EndLogLine;

  while( 1 )
    {
      int conn_rc = connect( drc_cli_socket,
           (struct sockaddr *) & (gDataReceiverThreadArgs.drc_cli_addr),
           sizeof( gDataReceiverThreadArgs.drc_cli_addr ) );
      int err=errno ;
      BegLogLine(FXLOG_IT_API_O_SOCKETS)
        << "conn_rc=" << conn_rc
        << " errno=" << errno
        << EndLogLine ;

      if( conn_rc == 0 )
  break;
      else if( conn_rc < 0 )
  {
    if( errno != EAGAIN )
      {
        perror( "connect failed" );
        StrongAssertLogLine( 0 )
    << "iWARPEM_DataReceiverThread:: Error after connect(): "
    << " errno: " << err
    << " conn_rc: " << conn_rc
    << EndLogLine;
      }
  }
    }
  pthread_mutex_lock( & gReceiveThreadStartedMutex );

  int rc = pthread_create( & DataReceiverTID,
                           NULL,
                           iWARPEM_DataReceiverThread,
                           (void *) & gDataReceiverThreadArgs );   

  pthread_mutex_lock( & gReceiveThreadStartedMutex );
  pthread_mutex_unlock( & gReceiveThreadStartedMutex );
  
  StrongAssertLogLine( rc == 0 )
    << "it_ia_create(): ERROR: "
    << " Failed to create a receiver thread "
    << " Errno " << errno    
    << EndLogLine;
  /***********************************************/
  
  


  /***********************************************
   * Accept connection from the DataReceiverThread
   ***********************************************/
#ifdef IT_API_OVER_UNIX_DOMAIN_SOCKETS
  struct sockaddr_un   drc_serv_addr_tmp;
#else
  struct sockaddr_in   drc_serv_addr_tmp;
#endif

//  sleep(1) ;
  socklen_t drc_serv_addr_tmp_len = sizeof( drc_serv_addr_tmp );
  BegLogLine(FXLOG_IT_API_O_SOCKETS)
    << "Before accept()"
    << EndLogLine ;

  int new_drc_serv_sock = accept( drc_serv_socket, 
				  (struct sockaddr *) & drc_serv_addr_tmp,
                                  & drc_serv_addr_tmp_len );
    
  StrongAssertLogLine( new_drc_serv_sock > 0 )
    << "it_ia_create(): after accept(): "
    << " errno: " << errno
    << " drc_serv_socket: " << drc_serv_socket
    << EndLogLine;
#if defined(SPINNING_RECEIVE)
  socket_nonblock_on(new_drc_serv_sock) ;
#endif

  BegLogLine(FXLOG_IT_API_O_SOCKETS)
    << "it_ia_create(): after accept(): "  
    << " new_drc_serv_sock: " << new_drc_serv_sock
    << " drc_serv_socket: " << drc_serv_socket
    << EndLogLine;

  gDataReceiverControlSockFd = new_drc_serv_sock;
  pthread_mutex_init( & gDataReceiverControlSockMutex, NULL );  
  close( drc_serv_socket ); 
  /***********************************************/



  /***********************************************
   * Start the data sender thread
   ***********************************************/
  pthread_mutex_lock( & gSendThreadStartedMutex );

  pthread_t DataSenderTID;

  rc = pthread_create( & DataSenderTID,
		       NULL,
		       iWARPEM_DataSenderThread,
		       (void *) NULL );   

  StrongAssertLogLine( rc == 0 )
    << "it_ia_create(): ERROR: "
    << " Failed to create a sender thread "
    << " Errno " << errno    
    << EndLogLine;

  pthread_mutex_lock( & gSendThreadStartedMutex );
  pthread_mutex_unlock( & gSendThreadStartedMutex );

  BegLogLine( FXLOG_IT_API_O_SOCKETS )
    << "it_ia_create(): After pthread_create of the sender thread by the main thread"
    << EndLogLine;

  /***********************************************/
    gITAPI_Initialized = 1;
    }

  *ia_handle = (it_ia_handle_t) ape_ia_handle_next;

  pthread_mutex_unlock( & gITAPI_INITMutex );

  return(IT_SUCCESS);
  }


// it_ia_free

it_status_t it_ia_free (
  IN  it_ia_handle_t ia_handle
  )
  {
  BegLogLine(FXLOG_IT_API_O_SOCKETS)
    << "it_ia_free()"
    << "IN it_ia_handle_t ia_handle " << ia_handle
    << EndLogLine;
  return(IT_SUCCESS);
  }


// U it_pz_create
static int ape_pz_handle_next = 0;

it_status_t it_pz_create (
  IN  it_ia_handle_t  ia_handle,
  OUT it_pz_handle_t *pz_handle
  )
  {
  pthread_mutex_lock( & gITAPIFunctionMutex );

  *pz_handle = (it_pz_handle_t) ape_pz_handle_next++;

  BegLogLine(FXLOG_IT_API_O_SOCKETS)
    << "it_pz_create(): "
    << " IN it_ia_handle "  << ia_handle
    << " OUT pz_handle "    << *pz_handle
    << EndLogLine;

  pthread_mutex_unlock( & gITAPIFunctionMutex );

  return(IT_SUCCESS);
  }

// it_pz_free

it_status_t it_pz_free (
  IN  it_pz_handle_t pz_handle
  )
  {
  BegLogLine(FXLOG_IT_API_O_SOCKETS)
    << "it_pz_free()"
    << "IN it_pz_handle_t pz_handle " << pz_handle
    << EndLogLine;
  return(IT_SUCCESS);
  }

// U it_ep_rc_create

it_status_t it_ep_rc_create (
  IN        it_pz_handle_t            pz_handle,
  IN        it_evd_handle_t           request_sevd_handle,
  IN        it_evd_handle_t           recv_sevd_handle,
  IN        it_evd_handle_t           connect_sevd_handle,
  IN        it_ep_rc_creation_flags_t flags,
  IN  const it_ep_attributes_t       *ep_attr,
  OUT       it_ep_handle_t           *ep_handle
  )
  {
  pthread_mutex_lock( & gITAPIFunctionMutex );

  BegLogLine(FXLOG_IT_API_O_SOCKETS) << "it_ep_rc_create()" << EndLogLine;

  /* TODO: Looks leaked */
  iWARPEM_Object_EndPoint_t* EPObj =
                  (iWARPEM_Object_EndPoint_t*) malloc( sizeof(iWARPEM_Object_EndPoint_t) );

  BegLogLine(FXLOG_IT_API_O_SOCKETS)
    << "EPObj malloc -> " << (void *) EPObj
    << EndLogLine ;
  StrongAssertLogLine( EPObj )
    << "it_ep_rc_create():"
    << " failed to allocate memory for EndPoint object "
    << EndLogLine;

  bzero( EPObj, sizeof( iWARPEM_Object_EndPoint_t ) );

  EPObj->pz_handle           = pz_handle;
  EPObj->request_sevd_handle = request_sevd_handle;
  EPObj->recv_sevd_handle    = recv_sevd_handle;
  EPObj->connect_sevd_handle = connect_sevd_handle;
  EPObj->flags               = flags;
  EPObj->ep_attr             = *ep_attr;
  EPObj->ep_handle           = (it_ep_handle_t) EPObj;

  EPObj->RecvWrQueue.Init( IWARPEM_RECV_WR_QUEUE_MAX_SIZE );   

  // EPObj->SendWrQueue.Init();
  // gSendWRLocalEndPointList.Insert( EPObj );

  *ep_handle = (it_ep_handle_t) EPObj;

  BegLogLine(FXLOG_IT_API_O_SOCKETS)
    << "it_ep_rc_create()"
    << " ep_handle "           << *((iWARPEM_Object_EndPoint_t *) *ep_handle)
    << " pz_handle           " << (void*) pz_handle
    << " request_sevd_handle " << (void*) request_sevd_handle
    << " recv_sevd_handle    " << (void*) recv_sevd_handle
    << " connect_sevd_handle " << (void*) connect_sevd_handle
    << " flags               " << (void*) flags
    << " ep_attr@            " << (void*) ep_attr
    << EndLogLine;

  BegLogLine(FXLOG_IT_API_O_SOCKETS) << "ep_attr->max_dto_payload_size           " << ep_attr->max_dto_payload_size           << EndLogLine;
  BegLogLine(FXLOG_IT_API_O_SOCKETS) << "ep_attr->max_request_dtos               " << ep_attr->max_request_dtos               << EndLogLine;
  BegLogLine(FXLOG_IT_API_O_SOCKETS) << "ep_attr->max_recv_dtos                  " << ep_attr->max_recv_dtos                  << EndLogLine;
  BegLogLine(FXLOG_IT_API_O_SOCKETS) << "ep_attr->max_send_segments              " << ep_attr->max_send_segments              << EndLogLine;
  BegLogLine(FXLOG_IT_API_O_SOCKETS) << "ep_attr->max_recv_segments              " << ep_attr->max_recv_segments              << EndLogLine;
  BegLogLine(FXLOG_IT_API_O_SOCKETS) << "ep_attr->srv.rc.rdma_read_enable        " << ep_attr->srv.rc.rdma_read_enable        << EndLogLine;
  BegLogLine(FXLOG_IT_API_O_SOCKETS) << "ep_attr->srv.rc.rdma_write_enable       " << ep_attr->srv.rc.rdma_write_enable       << EndLogLine;
  BegLogLine(FXLOG_IT_API_O_SOCKETS) << "ep_attr->srv.rc.max_rdma_read_segments  " << ep_attr->srv.rc.max_rdma_read_segments  << EndLogLine;
  BegLogLine(FXLOG_IT_API_O_SOCKETS) << "ep_attr->srv.rc.max_rdma_write_segments " << ep_attr->srv.rc.max_rdma_write_segments << EndLogLine;
  BegLogLine(FXLOG_IT_API_O_SOCKETS) << "ep_attr->srv.rc.rdma_read_ird           " << ep_attr->srv.rc.rdma_read_ird           << EndLogLine;
  BegLogLine(FXLOG_IT_API_O_SOCKETS) << "ep_attr->srv.rc.rdma_read_ord           " << ep_attr->srv.rc.rdma_read_ord           << EndLogLine;
  BegLogLine(FXLOG_IT_API_O_SOCKETS) << "ep_attr->srv.rc.srq                     " << ep_attr->srv.rc.srq                     << EndLogLine;
  BegLogLine(FXLOG_IT_API_O_SOCKETS) << "ep_attr->srv.rc.soft_hi_watermark       " << ep_attr->srv.rc.soft_hi_watermark       << EndLogLine;
  BegLogLine(FXLOG_IT_API_O_SOCKETS) << "ep_attr->srv.rc.hard_hi_watermark       " << ep_attr->srv.rc.hard_hi_watermark       << EndLogLine;
  BegLogLine(FXLOG_IT_API_O_SOCKETS) << "ep_attr->srv.rc.atomics_enable          " << ep_attr->srv.rc.atomics_enable          << EndLogLine;
  BegLogLine(FXLOG_IT_API_O_SOCKETS) << "ep_attr->priv_ops_enable                " << ep_attr->priv_ops_enable                << EndLogLine;

  pthread_mutex_unlock( & gITAPIFunctionMutex );

  return(IT_SUCCESS);
  }

// U it_evd_create


#define APE_MAX_EVD_HANDLES 1024
int ape_evd_handle_next = 0;

static itov_event_queue_t *CMQueue ;

it_status_t it_evd_create (
  IN  it_ia_handle_t   ia_handle,
  IN  it_event_type_t  event_number,
  IN  it_evd_flags_t   evd_flag,
  IN  size_t           sevd_queue_size,
  IN  size_t           sevd_threshold,
  IN  it_evd_handle_t  aevd_handle,
  OUT it_evd_handle_t *evd_handle,
  OUT int             *fd
  )
  {
  pthread_mutex_lock( & gITAPIFunctionMutex );

  BegLogLine(FXLOG_IT_API_O_SOCKETS) << "it_evd_create(): " << EndLogLine;

  iWARPEM_Object_EventQueue_t* EVQObj =
                  (iWARPEM_Object_EventQueue_t*) malloc( sizeof(iWARPEM_Object_EventQueue_t) );

  StrongAssertLogLine( EVQObj )
    << "it_evd_create():"
    << " failed to allocate memory for EventQueue object "
    << EndLogLine;

  BegLogLine(FXLOG_IT_API_O_SOCKETS_TYPES)
    << "About to init EVQObj=" << EVQObj
    << EndLogLine ;
  EVQObj->Init();
  BegLogLine(FXLOG_IT_API_O_SOCKETS_TYPES)
    << "Back from init EVQObj=" << EVQObj
    << EndLogLine ;

  EVQObj->ia_handle       = ia_handle;
  EVQObj->event_number    = event_number;
  EVQObj->evd_flag        = evd_flag;
  EVQObj->sevd_queue_size = sevd_queue_size;
  EVQObj->sevd_threshold  = sevd_threshold;
  EVQObj->aevd_handle     = aevd_handle;
  //EVQObj->fd              = fd;

  *evd_handle = (it_evd_handle_t) EVQObj;

  // EVQObj->mQueue.mMax = sevd_queue_size;

  BegLogLine(FXLOG_IT_API_O_SOCKETS) << "it_evd_create()                      " << EndLogLine;
  BegLogLine(FXLOG_IT_API_O_SOCKETS) << "IN  it_ia_handle_t   ia_handle       " <<  ia_handle        << EndLogLine;
  BegLogLine(FXLOG_IT_API_O_SOCKETS) << "IN  it_event_type_t  event_number    " <<  event_number     << EndLogLine;
  BegLogLine(FXLOG_IT_API_O_SOCKETS) << "IN  it_evd_flags_t   evd_flag        " <<  evd_flag         << EndLogLine;
  BegLogLine(FXLOG_IT_API_O_SOCKETS) << "IN  size_t           sevd_queue_size " <<  sevd_queue_size  << EndLogLine;
  BegLogLine(FXLOG_IT_API_O_SOCKETS) << "IN  size_t           sevd_threshold  " <<  sevd_threshold   << EndLogLine;
  BegLogLine(FXLOG_IT_API_O_SOCKETS) << "IN  it_evd_handle_t  aevd_handle     " <<  aevd_handle      << EndLogLine;
  BegLogLine(FXLOG_IT_API_O_SOCKETS) << "OUT it_evd_handle_t  evd_handle      " << *evd_handle << " @ " << (void*)evd_handle       << EndLogLine;
  BegLogLine(FXLOG_IT_API_O_SOCKETS) << "OUT int             *fd              " << (void*)fd          << EndLogLine;

  it_api_o_sockets_device_mgr_t* deviceMgr = (it_api_o_sockets_device_mgr_t *) ia_handle;

  if( event_number == IT_AEVD_NOTIFICATION_EVENT_STREAM )
    {
      if( itov_aevd_defined )
        {
          return IT_ERR_INVALID_EVD_STATE;
        }

      it_api_o_sockets_aevd_mgr_t* CQ = (it_api_o_sockets_aevd_mgr_t *) malloc( sizeof( it_api_o_sockets_aevd_mgr_t ) );
      StrongAssertLogLine( CQ )
        << "it_evd_create(): ERROR: "
        << EndLogLine;

      BegLogLine(FXLOG_IT_API_O_SOCKETS)
        << "CQ=" << CQ
        << EndLogLine ;

      bzero( CQ, sizeof( it_api_o_sockets_aevd_mgr_t ) );

      CQ->Init( deviceMgr );

      CMQueue = & CQ->mCMQueue;
      BegLogLine(FXLOG_IT_API_O_SOCKETS)
        << "CMQueue=" << CMQueue
        << EndLogLine ;
      // 1. Start all the pthreads
      // 2. Turn off non blocking behaviour
      CQ->mCMThreadArgs.mEventCmplQueue    = & CQ->mCMQueue;
      CQ->mCMThreadArgs.mMainCond          = & CQ->mMainCond;
      CQ->mCMThreadArgs.mEventCounterMutex = & CQ->mEventCounterMutex;
      CQ->mCMThreadArgs.mEventCounter      = & CQ->mEventCounter;
//      CQ->mCMThreadArgs.mCmChannel         = CQ->mDevice->cm_channel;

//      int rc = pthread_create( & CQ->mCMQueueTID,
//                               NULL,
//                               it_api_o_sockets_cm_processing_thread,
//                               (void *) & (CQ->mCMThreadArgs) );
//
      int rc=0 ;
      StrongAssertLogLine( rc == 0 )
        << "ERROR: "
        << " rc: " << rc
        << EndLogLine;

      for( int i=0; i < 1; i++ )
        {
//          it_status_t istatus = socket_nonblock_off( CQ->mDevice->devices[ i ]->async_fd );
//          if( istatus != IT_SUCCESS )
//            {
//              return istatus;
//            }
//
//          CQ->mAffThreadArgs[ i ].mEventCmplQueue = & CQ->mAffQueues[ i ];
//          CQ->mAffThreadArgs[ i ].mDevice         = CQ->mDevice->devices[ i ];
//
//          CQ->mAffThreadArgs[ i ].mMainCond          = & CQ->mMainCond;
//          CQ->mAffThreadArgs[ i ].mEventCounterMutex = & CQ->mEventCounterMutex;
//          CQ->mAffThreadArgs[ i ].mEventCounter      = & CQ->mEventCounter;
//
////          rc = pthread_create( & CQ->mAffQueuesTIDs[ i ],
////                               NULL,
////                               it_api_o_sockets_aff_processing_thread,
////                               (void *) & (CQ->mAffThreadArgs[ i ]) );
////
////          StrongAssertLogLine( rc == 0 )
////            << "ERROR: "
////            << " rc: " << rc
////            << EndLogLine;
//
//          CQ->mSendThreadArgs[ i ].mEventCmplQueue    = & CQ->mSendQueues[ i ];
//          CQ->mSendThreadArgs[ i ].mCQReadyMutex      = & CQ->mSendCQReadyMutexes[ i ];
//          CQ->mSendThreadArgs[ i ].mMainCond          = & CQ->mMainCond;
//          CQ->mSendThreadArgs[ i ].mEventCounterMutex = & CQ->mEventCounterMutex;
//          CQ->mSendThreadArgs[ i ].mEventCounter      = & CQ->mEventCounter;
          BegLogLine(FXLOG_IT_API_O_SOCKETS)
              << "& CQ->mSendQueues[ 0 ]=" << & CQ->mSendQueues[ 0 ]
              << EndLogLine ;
          gSendCmplQueue = & CQ->mSendQueues[ 0 ] ;
//
////          rc = pthread_create( & CQ->mSendQueuesTIDs[ i ],
////                               NULL,
////                               it_api_o_sockets_dto_processing_thread,
////                               (void *) & (CQ->mSendThreadArgs[ i ]) );
////
////          StrongAssertLogLine( rc == 0 )
////            << "ERROR: "
////            << " rc: " << rc
////            << EndLogLine;
//
//          CQ->mRecvThreadArgs[ i ].mEventCmplQueue    = & CQ->mRecvQueues[ i ];
//          CQ->mRecvThreadArgs[ i ].mCQReadyMutex      = & CQ->mRecvCQReadyMutexes[ i ];
//          CQ->mRecvThreadArgs[ i ].mMainCond          = & CQ->mMainCond;
//          CQ->mRecvThreadArgs[ i ].mEventCounterMutex = & CQ->mEventCounterMutex;
//          CQ->mRecvThreadArgs[ i ].mEventCounter      = & CQ->mEventCounter;
          BegLogLine(FXLOG_IT_API_O_SOCKETS)
            << "& CQ->mRecvQueues[ 0 ]=" << & CQ->mRecvQueues[ 0 ]
            << EndLogLine ;
          gRecvCmplQueue = & CQ->mRecvQueues[ 0 ] ;
//
////          rc = pthread_create( & CQ->mRecvQueuesTIDs[ i ],
////                               NULL,
////                               it_api_o_sockets_dto_processing_thread,
////                               (void *) & (CQ->mRecvThreadArgs[ i ]) );
////
////          StrongAssertLogLine( rc == 0 )
////            << "ERROR: "
////            << " rc: " << rc
////            << EndLogLine;
        }

      itov_aevd_defined = 1;
      *evd_handle = (it_evd_handle_t) CQ;

    }
  else if ( event_number == IT_CM_MSG_EVENT_STREAM)
    {
      it_api_o_sockets_aevd_mgr_t* CQ =(it_api_o_sockets_aevd_mgr_t*) aevd_handle ;
      BegLogLine(FXLOG_IT_API_O_SOCKETS)
        << "aevd handle=" << CQ
          << " CMM handle " << EVQObj
          << EndLogLine ;
      if(CQ != NULL) CQ->mCMMEVQObj = EVQObj ;
    }

//  else
//    {
//      it_api_o_sockets_cq_mgr_t* CQ = (it_api_o_sockets_cq_mgr_t *) malloc( sizeof( it_api_o_sockets_cq_mgr_t ) );
//      StrongAssertLogLine( CQ )
//        << "it_evd_create(): ERROR: "
//        << EndLogLine;
//      BegLogLine(FXLOG_IT_API_O_SOCKETS)
//        << "CQ=" << CQ
//        << EndLogLine ;
//
//      bzero( CQ, sizeof( it_api_o_sockets_cq_mgr_t ) );
//
//      CQ->event_number   = event_number;
//      CQ->queue_size     = sevd_queue_size;
//      CQ->device         = deviceMgr;
//      CQ->aevd           = (it_api_o_sockets_aevd_mgr_t *) aevd_handle;
//
//      // This field gets set letter when the QP is created
//      CQ->dto_type       = CQ_UNINITIALIZED;
//
//      switch( event_number )
//        {
//        case IT_ASYNC_UNAFF_EVENT_STREAM:
//        case IT_ASYNC_AFF_EVENT_STREAM:
//          {
//            break;
//          }
//        case IT_CM_REQ_EVENT_STREAM:
//        case IT_CM_MSG_EVENT_STREAM:
//          {
////            CQ->cq.cm_channel = deviceMgr->cm_channel;
//
//            break;
//          }
//        case IT_DTO_EVENT_STREAM:
//          {
////            int cqSize = sizeof( struct ibv_cq * ) * deviceMgr->devices_count;
////            CQ->cq.cq = (struct ibv_cq **) malloc( cqSize );
////            StrongAssertLogLine( CQ->cq.cq )
////              << "it_evd_create(): ERROR: "
////              << " cqSize: " << cqSize
////              << EndLogLine;
////
////            for( int i = 0; i < deviceMgr->devices_count; i++ )
////              {
////                CQ->cq.cq[ i ] = NULL;
////              }
//
//            break;
//          }
//        case IT_SOFTWARE_EVENT_STREAM:
//          {
//            break;
//          }
//        default:
//          {
//            StrongAssertLogLine( 0 )
//              << "it_evd_create(): ERROR: Unrecognized event number: "
//              << event_number
//              << EndLogLine;
//
//            break;
//          }
//        }
//
//      *evd_handle = (it_evd_handle_t) CQ;
//    }

  pthread_mutex_unlock( & gITAPIFunctionMutex );

  return(IT_SUCCESS);
  }


// it_evd_free

it_status_t it_evd_free (
  IN  it_evd_handle_t evd_handle
  )
  {
  BegLogLine(FXLOG_IT_API_O_SOCKETS)
    << "it_evd_free()"
    << "IN it_evd_handle_t evd_handle " << evd_handle
    << EndLogLine;
  return(IT_SUCCESS);
  }

// U it_evd_dequeue

it_status_t it_evd_dequeue (
  IN  it_evd_handle_t evd_handle, // Handle for simple or agregate queue
  OUT it_event_t     *event
  )
  {
  
  static unsigned int Count = 0;
  if( Count == 10000 )
    {
    BegLogLine(FXLOG_IT_API_O_SOCKETS) << "it_evd_dequeue(): evd_handle " << (void*) evd_handle << EndLogLine;
    }
  Count++;

  pthread_mutex_lock( & gITAPIFunctionMutex );

  StrongAssertLogLine( evd_handle != (it_evd_handle_t)NULL )
    << "it_evd_dequeue(): Handle is NULL "
    << EndLogLine

  iWARPEM_Object_EventQueue_t* EVQObj = (iWARPEM_Object_EventQueue_t *) evd_handle;

  iWARPEM_Object_Event_t *EventPtr;

  int rc = EVQObj->Dequeue( &EventPtr );

  if( rc == 0 )
    {
      AssertLogLine( EventPtr != NULL )
        << "it_evd_dequeue(): ERROR: EventPtr is NULL"
        << " EVQObj: " << (void *) EVQObj
        << " EVQObj->Queue: " << (void *) &(EVQObj->mQueue)
        << EndLogLine;
      
    *event = EventPtr->mEvent;
    
    BegLogLine( FXLOG_IT_API_O_SOCKETS )
      << "About to call free( " << (void *) EventPtr << " )"
      << EndLogLine;

    free( EventPtr );
    BegLogLine(FXLOG_IT_API_O_SOCKETS) << "it_evd_dequeue(): evd_handle " << (void*) evd_handle << " SUCCESS " << EndLogLine;
    
    pthread_mutex_unlock( & gITAPIFunctionMutex );
    return(IT_SUCCESS);
    }
  else
    {
    ////BegLogLine(FXLOG_IT_API_O_SOCKETS) << "it_evd_dequeue(): evd_handle " << (void*) evd_handle << " EMPTY " << rc << EndLogLine;
      pthread_mutex_unlock( & gITAPIFunctionMutex );
      return(IT_ERR_QUEUE_EMPTY);
    }
  }


it_status_t it_evd_dequeue_n(
  IN  it_evd_handle_t evd_handle,
  IN  int             deque_count,
  OUT it_event_t     *events,
  OUT int            *dequed_count )
{

  int DequeuedCount = 0;

  for( int i = 0; i < deque_count; i++ )
    {
      int rc = it_evd_dequeue( evd_handle, & events[ i ] );
      if( rc !=  IT_ERR_QUEUE_EMPTY )
        DequeuedCount++;      
      else
        break;
    }
  
  *dequed_count = DequeuedCount;
 
  return IT_SUCCESS;
}

// U it_evd_wait

it_status_t it_evd_wait (
  IN  it_evd_handle_t evd_handle,
  IN  uint64_t        timeout,
  OUT it_event_t     *event,
  OUT size_t         *nmore
  )
  {
  pthread_mutex_lock( & gITAPIFunctionMutex );

  BegLogLine(FXLOG_IT_API_O_SOCKETS)
    << "it_evd_wait()"
    << " evd_handle " << (void*) evd_handle
    << " timeout "    << timeout
    << " event@ "     << event
    << " nmore@ "     << nmore
    << EndLogLine;

  StrongAssertLogLine( evd_handle != (it_evd_handle_t)NULL )
    << "it_evd_wait(): Handle is NULL "
    << EndLogLine

  iWARPEM_Object_EventQueue_t* EVQObj = (iWARPEM_Object_EventQueue_t *) evd_handle;

  it_status_t rc;

  while( (rc = it_evd_dequeue( evd_handle, event )) == IT_ERR_QUEUE_EMPTY )
    {
      sleep(1);
    }

  BegLogLine(FXLOG_IT_API_O_SOCKETS)
    << "it_evd_wait()"
    << " evd_handle " << (void*) evd_handle
    << " timeout "    << timeout
    << " event@ "     << event
    << " rc "         << (void*) rc
    << EndLogLine;

  if( nmore )
    *nmore = EVQObj->mQueue.GetCount();

  pthread_mutex_unlock( & gITAPIFunctionMutex );

  return(rc);
  }


// U it_listen_create

// NEEDED FOR ASYNCRONOUS CONNECTIONS
struct iWARPEM_Object_ConReqInfo_t
  {    
    int                         ConnFd;
  };

struct iWARPEM_Object_Accept_t
  {
  // record all the args passed to it_listen_create
  it_ia_handle_t      ia_handle;
  size_t              spigot_id;
  it_evd_handle_t     connect_evd;  // this is where connect requests will be posted
  it_listen_flags_t   flags;
  it_conn_qual_t      conn_qual;
  it_listen_handle_t  listen_handle;
  // added info about listen infrastructure
  int       mListenSockFd;
  pthread_t mAcceptThreadId;
  };

void*
iWARPEM_AcceptThread( void* args )
  {
  BegLogLine(FXLOG_IT_API_O_SOCKETS)
    << "iWARPEM_AcceptThread(): Started!"
    << " STARTING : iWARPEM_ObjectAccept@ " << args
    << EndLogLine;

  pthread_mutex_unlock( & gAcceptThreadStartedMutex );

  iWARPEM_Object_Accept_t* AcceptObject = (iWARPEM_Object_Accept_t *) args;

  int ListenSockFd = AcceptObject->mListenSockFd;

  BegLogLine(FXLOG_IT_API_O_SOCKETS)
    << "iWARPEM_AcceptThread(): "
    << " AcceptObject " << (void*) AcceptObject
    << " ListenSockFd " << ListenSockFd
    << EndLogLine;

  // Loop over listen
#ifdef IT_API_OVER_UNIX_DOMAIN_SOCKETS
  struct sockaddr_un   cliaddr;
#else
  struct sockaddr_in   cliaddr;
#endif

  while( 1 )
    {
      socklen_t cliaddrlen = sizeof( cliaddr );
      
      BegLogLine(FXLOG_IT_API_O_SOCKETS)
	<< "iWARPEM_AcceptThread(): "
	<< " AcceptObject " << (void*) AcceptObject
	<< " Before accept() "
	<< " ListenSockFd " << ListenSockFd
	<< EndLogLine;
      
      // Accept going here
      int ConnFd = accept( ListenSockFd, (struct sockaddr *) &cliaddr, &cliaddrlen );
#if defined(SPINNING_RECEIVE)
      socket_nonblock_on(ConnFd) ;
#endif
      socket_nodelay_on(ConnFd) ;
      
      // create a connection request info object to hold
      // information to be used in responding to the req -- user will do reject or accept
      /* TODO: These look leaked */
      /* Think plugged now */
      iWARPEM_Object_ConReqInfo_t* ConReqInfo = (iWARPEM_Object_ConReqInfo_t*) malloc(sizeof(iWARPEM_Object_ConReqInfo_t));
//      BegLogLine(FXLOG_IT_API_O_SOCKETS)
//        << "ConReqInfo malloc -> " << (void *) ConReqInfo
//        << EndLogLine ;
      
      StrongAssertLogLine( ConReqInfo )
	<< "Could not get memory for ConReqInfo object"
	<< EndLogLine;
      
      ConReqInfo->ConnFd = ConnFd;      
      
      // create an event to to pass to the user
      /* TODO: These look leaked */
      iWARPEM_Object_Event_t* ConReqEvent = (iWARPEM_Object_Event_t*) malloc(sizeof(iWARPEM_Object_Event_t));
      
//      BegLogLine(FXLOG_IT_API_O_SOCKETS)
//      << "ConReqEvent malloc -> " << (void *) ConReqEvent
//      << EndLogLine ;

      StrongAssertLogLine( ConReqEvent )
	<< "Could not get memory for connection request event"
	<< EndLogLine;
      
      bzero( ConReqEvent, sizeof( iWARPEM_Object_Event_t ));
            
      it_conn_request_event_t* icre = (it_conn_request_event_t*) &ConReqEvent->mEvent;
            
      BegLogLine(FXLOG_IT_API_O_SOCKETS)
        << "ConReqEvent=" << ConReqEvent
        << " icre=" << icre
        << " event_number=" << IT_CM_REQ_CONN_REQUEST_EVENT
        << EndLogLine ;
      icre->event_number = IT_CM_REQ_CONN_REQUEST_EVENT;
      icre->cn_est_id    = (it_cn_est_identifier_t) ConReqInfo ;
      icre->evd          = AcceptObject->connect_evd; // i guess?
      
      int PrivateDataLen;
      int rlen;
      iWARPEM_Status_t rstat = read_from_socket( ConnFd, 
						 (char *) & PrivateDataLen, 
						 sizeof( int ),
						 & rlen );

      if ( rstat == IWARPEM_ERRNO_CONNECTION_CLOSED)
        {
          BegLogLine(FXLOG_IT_API_O_SOCKETS)
              << "Connection closed after accept"
              << " closing fd=" << ConnFd
              << EndLogLine ;
          close(ConnFd) ;
          free(ConReqEvent) ;
        }
      else
        {

          AssertLogLine( rstat == IWARPEM_SUCCESS )
            << "iWARPEM_AcceptThread(): ERROR: "
            << " rstat: " << rstat
            << EndLogLine;

          BegLogLine(FXLOG_IT_API_O_SOCKETS)
            << "PrivateDataLen=" << PrivateDataLen
            << EndLogLine ;
          if( PrivateDataLen > 0 )
          {
            icre->private_data_present = IT_TRUE;

            rstat = read_from_socket( ConnFd,
                    (char *) icre->private_data,
                    IT_MAX_PRIV_DATA,
                    & rlen );
            BegLogLine(FXLOG_IT_API_O_SOCKETS)
              << "private_data=" << HexDump( icre->private_data, IT_MAX_PRIV_DATA)
              << EndLogLine ;

            AssertLogLine( rstat == IWARPEM_SUCCESS )
              << "iWARPEM_AcceptThread(): ERROR: "
              << " rstat: " << rstat
              << EndLogLine;
          }
          else
            icre->private_data_present = IT_FALSE;

          // post this to the connection queue for the listen
          iWARPEM_Object_EventQueue_t* ConnectEventQueuePtr = (iWARPEM_Object_EventQueue_t*)AcceptObject->connect_evd;
    //     ConnectEventQueuePtr doesn't seeem initialised
          BegLogLine(FXLOG_IT_API_O_SOCKETS)
            << "ConnectEventQueuePtr=" << ConnectEventQueuePtr
            << " ConReqEvent=" << ConReqEvent
            << EndLogLine ;
          int enqrc = ConnectEventQueuePtr->Enqueue( ConReqEvent );


          StrongAssertLogLine( enqrc == 0 ) << "failed to enqueue connection request event" << EndLogLine;
          BegLogLine(FXLOG_IT_API_O_SOCKETS)
            << "iWARPEM_AcceptThread(): "
            << " ConReqInfo@ "    << (void*) ConReqInfo
            << " Posted to connect_evd@ " << (void*) AcceptObject->connect_evd
            << EndLogLine;
          BegLogLine(FXLOG_IT_API_O_SOCKETS)
            << "Posted to CMQueue=" << CMQueue
            << EndLogLine ;
    //      it_event_t *event =(it_event_t *)malloc(sizeof(it_event_t));
    //
    //      event->event_number=IT_CM_REQ_CONN_REQUEST_EVENT ;
    //      int enqrc2 = CMQueue->Enqueue( *event );
          int enqrc2=CMQueue->Enqueue(*(it_event_t *) icre) ;
          StrongAssertLogLine( enqrc2 == 0 ) << "failed to enqueue connection request event" << EndLogLine;
          free(ConReqEvent) ; /* So efence will trace it */
          it_api_o_sockets_signal_accept() ;

        }
    }
  
  return NULL;
  }

it_status_t it_listen_create (
  IN  it_ia_handle_t      ia_handle,
  IN  size_t              spigot_id,
  IN  it_evd_handle_t     connect_evd,
  IN  it_listen_flags_t   flags,
  IN  OUT it_conn_qual_t *conn_qual,
  OUT it_listen_handle_t *listen_handle
  )
  {
  pthread_mutex_lock( & gITAPIFunctionMutex );

  iWARPEM_Object_Accept_t* AcceptObject =
                  (iWARPEM_Object_Accept_t*) malloc( sizeof(iWARPEM_Object_Accept_t) );
  StrongAssertLogLine( AcceptObject )
    << "it_listen_create():"
    << " failed to allocate memory for AcceptObject "
    << EndLogLine;

  AcceptObject->ia_handle   = ia_handle;
  AcceptObject->spigot_id   = spigot_id;
  AcceptObject->connect_evd = connect_evd;
  AcceptObject->flags       = flags;
  AcceptObject->conn_qual   = *conn_qual;
  AcceptObject->listen_handle  = (it_listen_handle_t) AcceptObject;
  AcceptObject->mAcceptThreadId = (pthread_t) 0xFFFFFFFF; // set later

  BegLogLine(FXLOG_IT_API_O_SOCKETS) << "it_listen_create() "  << " AcceptObject " << (void*) AcceptObject << EndLogLine;
  BegLogLine(FXLOG_IT_API_O_SOCKETS) << "IN  it_ia_handle_t      ia_handle     " <<  ia_handle       << EndLogLine;
  BegLogLine(FXLOG_IT_API_O_SOCKETS) << "IN  size_t              spigot_id     " <<  spigot_id       << EndLogLine;
  BegLogLine(FXLOG_IT_API_O_SOCKETS) << "IN  it_evd_handle_t     connect_evd   " <<  connect_evd     << EndLogLine;
  BegLogLine(FXLOG_IT_API_O_SOCKETS) << "IN  it_listen_flags_t   flags         " <<  flags           << EndLogLine;
  BegLogLine(FXLOG_IT_API_O_SOCKETS) << "IN  OUT it_conn_qual_t  conn_qual..>port.local " << conn_qual->conn_qual.lr_port.local << EndLogLine;
  BegLogLine(FXLOG_IT_API_O_SOCKETS) << "OUT it_listen_handle_t *listen_handle " << *listen_handle   << EndLogLine;

  struct sockaddr    *saddr;
  int s;
  socklen_t addrlen;

#ifdef IT_API_OVER_UNIX_DOMAIN_SOCKETS
  struct sockaddr_un   addr;
  bzero( (char *) &addr, sizeof( addr ) );

  addr.sun_family      = IT_API_SOCKET_FAMILY;
  
  sprintf( addr.sun_path, 
           "%s.%d", 
           IT_API_UNIX_SOCKET_PREFIX_PATH,
           conn_qual->conn_qual.lr_port.local );
  
  unlink( addr.sun_path );
  
  addrlen = sizeof( addr.sun_family ) + strlen( addr.sun_path );
#else
  struct sockaddr_in   addr;
  bzero( (char *) &addr, sizeof( addr ) );

  addr.sin_family      = IT_API_SOCKET_FAMILY;
  addr.sin_port        = conn_qual->conn_qual.lr_port.local;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  
  addrlen = sizeof( addr );
#endif

  saddr = (struct sockaddr *)&addr;

  if((s = socket(IT_API_SOCKET_FAMILY, SOCK_STREAM, 0)) < 0)
    {
    perror("udp socket() open");
    }

  StrongAssertLogLine( s >= 0 )
    << "it_listen_create(): "
    << " AcceptObject " << (void*) AcceptObject
    << " Failed to create UDP socket "
    << " Errno " << errno
    << EndLogLine;

  int SockSendBuffSize = -1;
  // BGF size_t ArgSize = sizeof( int );
  socklen_t ArgSize = sizeof( int );
  int SockRecvBuffSize = -1;
  getsockopt( s, SOL_SOCKET, SO_SNDBUF, (int *) & SockSendBuffSize, & ArgSize );
  getsockopt( s, SOL_SOCKET, SO_RCVBUF, (int *) & SockRecvBuffSize, & ArgSize );

  BegLogLine( 0 )
    << "it_listen_create(): "
    << " SockSendBuffSize: " << SockSendBuffSize
    << " SockRecvBuffSize: " << SockRecvBuffSize
    << EndLogLine;
  
  SockSendBuffSize = IT_API_SOCKET_BUFF_SIZE;
  SockRecvBuffSize = IT_API_SOCKET_BUFF_SIZE;
  setsockopt( s, SOL_SOCKET, SO_SNDBUF, (const char *) & SockSendBuffSize, ArgSize );
  setsockopt( s, SOL_SOCKET, SO_RCVBUF, (const char *) & SockRecvBuffSize, ArgSize );
  
  int True = 1;
  setsockopt( s, SOL_SOCKET, SO_REUSEADDR, (char *)&True, sizeof( True ) );

  int brc;

  if( brc = bind( s, saddr, addrlen ) < 0 )
    {
    perror("udp socket bind()");
    close(s);
    }

  StrongAssertLogLine( brc >= 0 )
    << "it_listen_create(): "
    << " AcceptObject " << (void*) AcceptObject
    << " Failed to bind UDP socket "
    << " Errno " << errno
    << EndLogLine;

#ifdef IT_API_OVER_UNIX_DOMAIN_SOCKETS
  BegLogLine(FXLOG_IT_API_O_SOCKETS)
    << "it_listen_create(): "
    << " s: " << s
    << " AcceptObject " << (void*) AcceptObject
    << " After bind()/getsockname() "
    << " addr.sun_path "        << addr.sun_path
    << EndLogLine;
#else
  BegLogLine(FXLOG_IT_API_O_SOCKETS)
    << "it_listen_create(): "
    << " s: " << s
    << " AcceptObject " << (void*) AcceptObject
    << " After bind()/getsockname() "
    << " addr.sin_port "        << (void*) addr.sin_port
    << " addr.sin_addr.s_addr " << (void*) addr.sin_addr.s_addr
    << EndLogLine;
#endif

  AcceptObject->mListenSockFd = s;

  int ListenRc = listen( s, 2048 );
  StrongAssertLogLine( ListenRc == 0 )
    << "iWARPEM_AcceptThread(): "
    << " ListenRc: " << ListenRc
    << " errno: " << errno
    << " s: " << s
    << EndLogLine;

  pthread_t tid;
  pthread_mutex_lock( & gAcceptThreadStartedMutex );

  int rc = pthread_create( & tid,
                           NULL,
                           iWARPEM_AcceptThread,
                           (void*) AcceptObject );

  StrongAssertLogLine( rc == 0 )
    << "it_ia_listen(): "
    << " AcceptObject " << (void*) AcceptObject
    << " ERROR:: Failed in pthread_create()"
    << " rc: " << rc
    << EndLogLine;

  pthread_mutex_lock( & gAcceptThreadStartedMutex );
  pthread_mutex_unlock( & gAcceptThreadStartedMutex );

  AcceptObject->mAcceptThreadId = tid;

  BegLogLine(FXLOG_IT_API_O_SOCKETS)
    << "it_ia_listen(): "
    << " AcceptObject " << (void*) AcceptObject
    << " created AcceptThread tid " << (void*) tid
    << EndLogLine;

  sched_yield();

  pthread_mutex_unlock( & gITAPIFunctionMutex );

  return(IT_SUCCESS);
  }

it_status_t it_ep_disconnect (
  IN        it_ep_handle_t ep_handle,
  IN  const unsigned char *private_data,
  IN        size_t         private_data_length
)
{
  pthread_mutex_lock( & gITAPIFunctionMutex );

  // BegLogLine(FXLOG_IT_API_O_SOCKETS) << "it_ep_disconnect()" << EndLogLine;
  BegLogLine( FXLOG_IT_API_O_SOCKETS ) << "it_ep_disconnect() Entering... " << EndLogLine;

  BegLogLine( FXLOG_IT_API_O_SOCKETS ) << "IN        it_ep_handle_t        ep_handle,                 " <<  *(iWARPEM_Object_EndPoint_t *)ep_handle           << EndLogLine;
  BegLogLine(FXLOG_IT_API_O_SOCKETS) << "IN  const unsigned char         private_data@              " <<  (void*)private_data        << EndLogLine;
  BegLogLine(FXLOG_IT_API_O_SOCKETS) << "IN        size_t                private_data_length        " <<   private_data_length << EndLogLine;

  iWARPEM_Object_EndPoint_t* EPObj = (iWARPEM_Object_EndPoint_t *) ep_handle;

  AssertLogLine( EPObj->ConnectedFlag == IWARPEM_CONNECTION_FLAG_CONNECTED )
    << "it_ep_disconnect(): ERROR:: it_ep_disconnect() called on a non connected EP"
    << " ep_handle: " <<  *(iWARPEM_Object_EndPoint_t *)ep_handle
    << EndLogLine;
    
  EPObj->ConnectedFlag = IWARPEM_CONNECTION_FLAG_ACTIVE_SIDE_PENDING_DISCONNECT;
  
  iwarpem_flush_queue( EPObj, IWARPEM_FLUSH_SEND_QUEUE_FLAG );

  iWARPEM_Object_WorkRequest_t *SendWR =
    (iWARPEM_Object_WorkRequest_t*) malloc( sizeof(iWARPEM_Object_WorkRequest_t) );
  
  StrongAssertLogLine( SendWR )
    << "it_ep_disconnect():"
    << " failed to allocate memory for Send work request object "
    << EndLogLine;

  bzero( SendWR, sizeof( iWARPEM_Object_WorkRequest_t ) );

  SendWR->ep_handle              = ep_handle;
  SendWR->mMessageHdr.mMsg_Type  = iWARPEM_DISCONNECT_REQ_TYPE;
  SendWR->mMessageHdr.EndianConvert() ;
  SendWR->segments_array         = NULL;

  // iWARPEM_Object_EndPoint_t* LocalEndPoint = (iWARPEM_Object_EndPoint_t * ) ep_handle;
  // LocalEndPoint->SendWrQueue.Enqueue( SendWR );
  
  iwarpem_enqueue_send_wr( gSendWrQueue, SendWR );

  // gSendWrQueue->Enqueue( SendWR );

  BegLogLine( FXLOG_IT_API_O_SOCKETS )
    << "it_ep_disconnect(): Enqueued a SendWR  request on: "    
    << " ep_handle: " << *(iWARPEM_Object_EndPoint_t *)ep_handle
    << " SendWR: " << (void *) SendWR
    << EndLogLine;

  BegLogLine( FXLOG_IT_API_O_SOCKETS ) << "it_ep_disconnect() Leaving... " << EndLogLine;

  pthread_mutex_unlock( & gITAPIFunctionMutex );

  return IT_SUCCESS;
}


// U it_ep_connect

// An object is needed to hold state between
// the call to connect and the generation of
// a connection event in response to this call.
it_status_t it_ep_connect (
  IN        it_ep_handle_t        ep_handle,
  IN  const it_path_t            *path,
  IN  const it_conn_attributes_t *conn_attr,
  IN  const it_conn_qual_t       *connect_qual,
  IN        it_cn_est_flags_t     cn_est_flags,
  IN  const unsigned char        *private_data,
  IN        size_t                private_data_length
  )
  {
  pthread_mutex_lock( & gITAPIFunctionMutex );

  BegLogLine(FXLOG_IT_API_O_SOCKETS) << "it_ep_connect()" << EndLogLine;

  BegLogLine(FXLOG_IT_API_O_SOCKETS) << "IN        it_ep_handle_t        ep_handle,                 " <<  *(iWARPEM_Object_EndPoint_t *)ep_handle           << EndLogLine;
  BegLogLine(FXLOG_IT_API_O_SOCKETS) << "IN  const it_path_t             path..>raddr.ipv4.s_addr   " <<  (void*) path->u.iwarp.raddr.ipv4.s_addr        << EndLogLine;
  BegLogLine(FXLOG_IT_API_O_SOCKETS) << "IN  const it_conn_attributes_t* conn_attr@                 " <<  (void*) conn_attr                              << EndLogLine;
  BegLogLine(FXLOG_IT_API_O_SOCKETS) << "IN  const it_conn_qual_t        connect_qual..>port.local  " <<  (void*) connect_qual->conn_qual.lr_port.local  << EndLogLine;
  BegLogLine(FXLOG_IT_API_O_SOCKETS) << "IN  const it_conn_qual_t        connect_qual..>port.remote " <<  (void*) connect_qual->conn_qual.lr_port.remote << EndLogLine;
  BegLogLine(FXLOG_IT_API_O_SOCKETS) << "IN        it_cn_est_flags_t     cn_est_flags,              " <<  (void*)  cn_est_flags        << EndLogLine;
  BegLogLine(FXLOG_IT_API_O_SOCKETS) << "IN  const unsigned char         private_data@              " <<  (void*)private_data        << EndLogLine;
  BegLogLine(FXLOG_IT_API_O_SOCKETS) << "IN        size_t                private_data_length        " <<   private_data_length << EndLogLine;
  
  if( private_data_length > IT_MAX_PRIV_DATA )
    {
      pthread_mutex_unlock( & gITAPIFunctionMutex );

      return IT_ERR_INVALID_PDATA_LENGTH;
    }

  iWARPEM_Object_EndPoint_t* LocalEndPoint = (iWARPEM_Object_EndPoint_t*) ep_handle;
  
  StrongAssertLogLine( LocalEndPoint != 0 )
    << "it_ep_connect(): local endpoint handle null"
    << EndLogLine;

  StrongAssertLogLine( LocalEndPoint->ConnectedFlag == IWARPEM_CONNECTION_FLAG_DISCONNECTED )
    << "it_ep_connect(): local endpoint already connected"
    << EndLogLine;

  int s;
  
  if((s = socket(IT_API_SOCKET_FAMILY, SOCK_STREAM, 0)) < 0)
    {
    perror("it_ep_connect socket() open");
    }
  
#ifdef IT_API_OVER_UNIX_DOMAIN_SOCKETS
  struct sockaddr_un serv_addr;
  
  memset( &serv_addr, 0, sizeof( serv_addr ) );
  serv_addr.sun_family      = IT_API_SOCKET_FAMILY;

  sprintf( serv_addr.sun_path, 
           "%s.%d", 
           IT_API_UNIX_SOCKET_PREFIX_PATH,
           connect_qual->conn_qual.lr_port.remote );

  socklen_t serv_addr_len = sizeof( serv_addr.sun_family ) + strlen( serv_addr.sun_path );
#else
  struct sockaddr_in serv_addr;
  
  memset( &serv_addr, 0, sizeof( serv_addr ) );
  serv_addr.sin_family      = IT_API_SOCKET_FAMILY;
  serv_addr.sin_port        = connect_qual->conn_qual.lr_port.remote;
  serv_addr.sin_addr.s_addr = path->u.iwarp.raddr.ipv4.s_addr;

  socklen_t serv_addr_len = sizeof( serv_addr );
#endif

  int SockSendBuffSize = -1;
  int SockRecvBuffSize = -1;
  //BGF size_t ArgSize = sizeof( int );
  socklen_t ArgSize = sizeof( int );
  getsockopt( s, SOL_SOCKET, SO_SNDBUF, (int *) & SockSendBuffSize, & ArgSize );
  getsockopt( s, SOL_SOCKET, SO_RCVBUF, (int *) & SockRecvBuffSize, & ArgSize );
  
  BegLogLine( 0 )
    << "it_ep_connect(): "
    << " SockSendBuffSize: " << SockSendBuffSize
    << " SockRecvBuffSize: " << SockRecvBuffSize
    << EndLogLine;

  SockSendBuffSize = IT_API_SOCKET_BUFF_SIZE;
  SockRecvBuffSize = IT_API_SOCKET_BUFF_SIZE;
  setsockopt( s, SOL_SOCKET, SO_SNDBUF, (const char *) & SockSendBuffSize, ArgSize );
  setsockopt( s, SOL_SOCKET, SO_RCVBUF, (const char *) & SockRecvBuffSize, ArgSize );
 
  while( 1 )
    {
      int ConnRc = connect( s, 
			    (struct sockaddr *) & serv_addr,
			    serv_addr_len );

      if( ConnRc < 0 )
	{
	if( errno != EAGAIN )
	  {
	    perror("Connection failed") ;
	    StrongAssertLogLine( 0 )
	      << "it_ep_connect:: Connection failed "
	      << " errno: " << errno
	      << EndLogLine;
	  }
	}
      else if( ConnRc == 0 )
	break;
    }
#if defined(SPINNING_RECEIVE)
  socket_nonblock_on(s) ;
#endif
  socket_nodelay_on(s) ;
  
  iWARPEM_Private_Data_t PrivateData;
  PrivateData.mLen = private_data_length;
  memcpy( PrivateData.mData, 
	  private_data,
	  private_data_length);

  int wLen;
  int SizeToSend = sizeof( iWARPEM_Private_Data_t );
  iWARPEM_Status_t wstat = write_to_socket( s, 
					    (char *) & PrivateData,
					    SizeToSend,
					    & wLen );
  AssertLogLine( wLen == SizeToSend ) 
    << "it_ep_connect(): ERROR: "
    << " wLen: " << wLen
    << " SizeToSend: " << SizeToSend
    << EndLogLine;

  LocalEndPoint->ConnectedFlag = IWARPEM_CONNECTION_FLAG_CONNECTED;
  LocalEndPoint->ConnFd        = s;

  StrongAssertLogLine( private_data_length > 0 )
    << "it_ep_connect(): ERROR: private_data must be set to the other EP Node Id (needed for debugging)"
    << EndLogLine;
  
  int Dummy;
  sscanf( (const char *) private_data, "%d %d", &Dummy, &(LocalEndPoint->OtherEPNodeId) );   

  // Add the socket descriptor to the data receiver controller
  iwarpem_add_socket_to_list( s, LocalEndPoint );

  // Generate the connection established event
  iWARPEM_Object_Event_t* ConnEstablishedEvent = (iWARPEM_Object_Event_t*) malloc( sizeof( iWARPEM_Object_Event_t ) );
  
  it_connection_event_t* ice = (it_connection_event_t*) & ConnEstablishedEvent->mEvent;  
  
  ice->event_number = IT_CM_MSG_CONN_ESTABLISHED_EVENT;
  ice->evd          = LocalEndPoint->connect_sevd_handle; // i guess?
  ice->ep           = (it_ep_handle_t) ep_handle;
  
  iWARPEM_Object_EventQueue_t* ConnectEventQueuePtr = 
    (iWARPEM_Object_EventQueue_t*) LocalEndPoint->connect_sevd_handle;

// Queue doesn't seem initialised
//  int enqrc = ConnectEventQueuePtr->Enqueue( ConnEstablishedEvent );
//
//  StrongAssertLogLine( enqrc == 0 ) << "failed to enqueue connection request event" << EndLogLine;
  
  pthread_mutex_unlock( & gITAPIFunctionMutex );

  return(IT_SUCCESS);
  }


// U it_ep_accept
it_status_t it_ep_accept (
  IN        it_ep_handle_t         ep_handle,
  IN        it_cn_est_identifier_t cn_est_id,
  IN  const unsigned char         *private_data,
  IN        size_t                 private_data_length
  )
  {
    // pthread_mutex_lock( & gITAPIFunctionMutex );

  // this is the moment when the ConReq is first associated with the passive side endpoint
  BegLogLine(FXLOG_IT_API_O_SOCKETS) << "it_ep_accept()" << EndLogLine;
  BegLogLine(FXLOG_IT_API_O_SOCKETS) << "it_ep_handle_t         ep_handle           " << *(iWARPEM_Object_EndPoint_t *)ep_handle   << EndLogLine;
  BegLogLine(FXLOG_IT_API_O_SOCKETS) << "it_cn_est_identifier_t cn_est_id           " << cn_est_id           << EndLogLine;
  BegLogLine(FXLOG_IT_API_O_SOCKETS) << "unsigned char         *private_data @      " << (void*)private_data << EndLogLine;
  BegLogLine(FXLOG_IT_API_O_SOCKETS) << "size_t                 private_data_length " << private_data_length << EndLogLine;

  // Use the cn_est_id to get back to information about this connection request
  iWARPEM_Object_ConReqInfo_t* ConReqInfoPtr = (iWARPEM_Object_ConReqInfo_t*) cn_est_id;

  // Actually should check magic and other values too
//   ConReqInfoPtr only required for its ConnFd, which in the sockets implementation
//   is handled by the underlying accept support
//  StrongAssertLogLine( ConReqInfoPtr != 0 ) << "it_ep_accept(): cn_est_id let to a NULL ConReqInfo object" << EndLogLine;

  // ConReqInfoPtr->AcceptingEndPoint  = ep_handle;
  // ConReqInfoPtr->AcceptRejectStatus = 1; // 1 is accepted

  // fill out our ep stuff
  iWARPEM_Object_EndPoint_t* LocalEndPoint = (iWARPEM_Object_EndPoint_t*) ep_handle;

  StrongAssertLogLine( LocalEndPoint != 0 )
    << "it_ep_accept(): local endpoint handle null"
    << EndLogLine;

  StrongAssertLogLine( LocalEndPoint->ConnectedFlag == IWARPEM_CONNECTION_FLAG_DISCONNECTED )
    << "it_ep_accept(): local endpoint already connected"
    << EndLogLine;

  LocalEndPoint->ConnectedFlag = IWARPEM_CONNECTION_FLAG_CONNECTED;
  LocalEndPoint->ConnFd        = ConReqInfoPtr->ConnFd;

  StrongAssertLogLine( private_data_length > 0 )
    << "it_ep_accept(): ERROR: private_data must be set to the other EP Node Id (needed for debugging)"
    << EndLogLine;

  sscanf( (const char *) private_data, "%d", &(LocalEndPoint->OtherEPNodeId) );   

  BegLogLine(FXLOG_IT_API_O_SOCKETS)
    << "it_ep_accept(): ep " << *(iWARPEM_Object_EndPoint_t *)ep_handle
    << " ConReqInfo@ " << (void*) ConReqInfoPtr
    << " SocketFd: " << ConReqInfoPtr->ConnFd
    << EndLogLine;

  // Add the socket descriptor to the data receiver controller
  iwarpem_add_socket_to_list( ConReqInfoPtr->ConnFd, LocalEndPoint );

  // create an event to to pass to the user
  iWARPEM_Object_Event_t* ConnEstablishedEvent = (iWARPEM_Object_Event_t*) malloc( sizeof( iWARPEM_Object_Event_t ) );

  it_connection_event_t* ice = (it_connection_event_t*) & ConnEstablishedEvent->mEvent;  
  
  ice->event_number = IT_CM_MSG_CONN_ESTABLISHED_EVENT;
  ice->evd          = LocalEndPoint->connect_sevd_handle; // i guess?
  ice->cn_est_id    = (it_cn_est_identifier_t) cn_est_id;
  ice->ep           = (it_ep_handle_t) LocalEndPoint;
  
  iWARPEM_Object_EventQueue_t* ConnectEventQueuePtr = 
    (iWARPEM_Object_EventQueue_t*) LocalEndPoint->connect_sevd_handle;

// Don't seem to have a ConnectEventQueuePtr->
  int enqrc = ConnectEventQueuePtr->Enqueue( ConnEstablishedEvent );

  StrongAssertLogLine( enqrc == 0 ) << "failed to enqueue connection request event" << EndLogLine;
  
  BegLogLine(FXLOG_IT_API_O_SOCKETS)
    << "it_ep_accept(): "
    << " ConReqInfo@ "    << (void*) ConReqInfoPtr
    << EndLogLine;     
  
  //  pthread_mutex_unlock( & gITAPIFunctionMutex );

  return(IT_SUCCESS);
  }


// it_ep_free

it_status_t it_ep_free (
  IN  it_ep_handle_t ep_handle
  )
  {
  pthread_mutex_lock( & gITAPIFunctionMutex );

  BegLogLine(FXLOG_IT_API_O_SOCKETS) << "it_ep_free()" << EndLogLine;
  BegLogLine(FXLOG_IT_API_O_SOCKETS) << "IN        it_ep_handle_t        ep_handle,          " <<   *(iWARPEM_Object_EndPoint_t *)ep_handle           << EndLogLine;
  iWARPEM_Object_EndPoint_t* EPObj = (iWARPEM_Object_EndPoint_t *) ep_handle;

  EPObj->RecvWrQueue.Finalize();    

  free( EPObj ); 

  pthread_mutex_unlock( & gITAPIFunctionMutex );

  return(IT_SUCCESS);
  }


// U it_lmr_create21
/*
   it_lmr_create21 provides the v2.1 functionality
   and may be renamed to it_lmr_create in a future IT-API version.
*/

it_status_t it_lmr_create21 (
  IN  it_pz_handle_t        pz_handle,
  IN  void                 *addr,
  IN  it_iobl_t            *iobl,
  IN  it_length_t           length,
  IN  it_addr_mode_t        addr_mode,
  IN  it_mem_priv_t         privs,
  IN  it_lmr_flag_t         flags,
  IN  uint32_t              shared_id,
  OUT it_lmr_handle_t      *lmr_handle,
  IN  OUT it_rmr_context_t *rmr_context
  )
  {

  pthread_mutex_lock( & gITAPIFunctionMutex );

  BegLogLine(FXLOG_IT_API_O_SOCKETS) << "it_lmr_create():       " << EndLogLine;

  iWARPEM_Object_MemoryRegion_t* MRObj =
                  (iWARPEM_Object_MemoryRegion_t*) malloc( sizeof(iWARPEM_Object_MemoryRegion_t) );
  BegLogLine(FXLOG_IT_API_O_SOCKETS)
    << "MRObj malloc -> " << (void *) MRObj
    << EndLogLine ;

  StrongAssertLogLine( MRObj )
    << "it_lmr_create():"
    << " failed to allocate memory for MemoryRegion object "
    << EndLogLine;

  MRObj->pz_handle   = pz_handle;
  MRObj->addr        = addr;
  ///////  MRObj->iobl        = ((iobl == NULL)? 0 : *iobl);
  MRObj->length      = length;
  MRObj->addr_mode   = addr_mode;
  MRObj->privs       = privs;
  MRObj->flags       = flags;
  MRObj->shared_id   = shared_id;
  MRObj->lmr_handle  = (it_lmr_handle_t) MRObj;
  MRObj->rmr_context = (it_rmr_context_t) MRObj;

  *lmr_handle = (it_lmr_handle_t) MRObj;

  if( rmr_context != NULL )
    *rmr_context = (it_rmr_context_t) MRObj;

  BegLogLine(FXLOG_IT_API_O_SOCKETS) << "it_lmr_create21():       " << EndLogLine;
  BegLogLine(FXLOG_IT_API_O_SOCKETS) << " IN  it_pz_handle_t        pz_handle   " << pz_handle << EndLogLine;
  BegLogLine(FXLOG_IT_API_O_SOCKETS) << " IN  void                 *addr        " << addr << EndLogLine;
// following line segvs
//  BegLogLine(FXLOG_IT_API_O_SOCKETS) << " IN  it_iobl_t            *iobl,       " << iobl->num_elts << " fbo " << iobl->fbo << EndLogLine;
  BegLogLine(FXLOG_IT_API_O_SOCKETS) << " IN  it_length_t           length,     " << length << EndLogLine;
  BegLogLine(FXLOG_IT_API_O_SOCKETS) << " IN  it_addr_mode_t        addr_mode,  " << addr_mode << EndLogLine;
  BegLogLine(FXLOG_IT_API_O_SOCKETS) << " IN  it_mem_priv_t         privs,      " << privs << EndLogLine;
  BegLogLine(FXLOG_IT_API_O_SOCKETS) << " IN  it_lmr_flag_t         flags,      " << flags << EndLogLine;
  BegLogLine(FXLOG_IT_API_O_SOCKETS) << " IN  uint32_t              shared_id,  " << shared_id << EndLogLine;
  BegLogLine(FXLOG_IT_API_O_SOCKETS) << " OUT it_lmr_handle_t      *lmr_handle, " << (void*) *lmr_handle << EndLogLine;
  BegLogLine(FXLOG_IT_API_O_SOCKETS) << " IN  OUT it_rmr_context_t *rmr_context " << (rmr_context ? (void*)*rmr_context : 0) << EndLogLine;

  pthread_mutex_unlock( & gITAPIFunctionMutex );

  return(IT_SUCCESS);
  }


// it_lmr_free

it_status_t it_lmr_free (
  IN  it_lmr_handle_t lmr_handle
  )
  {
  pthread_mutex_lock( & gITAPIFunctionMutex );

  BegLogLine(FXLOG_IT_API_O_SOCKETS)
    << "it_lmr_free()"
    << "IN it_lmr_handle_t lmr_handle " << lmr_handle
    << EndLogLine;

  if( lmr_handle != NULL )
    {
      iWARPEM_Object_MemoryRegion_t* MRObj =
	(iWARPEM_Object_MemoryRegion_t*) lmr_handle;
      
      bzero( MRObj, sizeof( iWARPEM_Object_MemoryRegion_t ) );

      BegLogLine( FXLOG_IT_API_O_SOCKETS )
	<< "About to call free( " << (void *) lmr_handle << " )"
	<< EndLogLine;

      free( lmr_handle );
    }

  pthread_mutex_unlock( & gITAPIFunctionMutex );

  return(IT_SUCCESS);
  }

it_status_t it_post_rdma_read (
  IN        it_ep_handle_t    ep_handle,
  IN  const it_lmr_triplet_t *local_segments,
  IN        size_t            num_segments,
  IN        it_dto_cookie_t   cookie,
  IN        it_dto_flags_t    dto_flags,
  IN        it_rdma_addr_t    rdma_addr,
  IN        it_rmr_context_t  rmr_context
  )
{
  pthread_mutex_lock( & gITAPIFunctionMutex );

  BegLogLine(FXLOG_IT_API_O_SOCKETS) << "it_post_rdma_read(): " << EndLogLine;
  BegLogLine(FXLOG_IT_API_O_SOCKETS) << "it_ep_handle_t   "      <<  *(iWARPEM_Object_EndPoint_t *)ep_handle << EndLogLine;
  BegLogLine(FXLOG_IT_API_O_SOCKETS) << "it_lmr_triplet_t "
				     << " @ "           << (void*) local_segments
				     << "->lmr (handle) " << (void*) local_segments->lmr
				     << "->addr->abs "  << (void*) local_segments->addr.abs
				     << "->length "     << local_segments->length
				     << EndLogLine;
  BegLogLine(FXLOG_IT_API_O_SOCKETS) << "size_t           " << num_segments      << EndLogLine;
  BegLogLine(FXLOG_IT_API_O_SOCKETS) << "it_dto_cookie_t  { " 
				     << cookie.mFirst << " , "
				     << cookie.mSecond 
				     << " } "
				     << EndLogLine;
  BegLogLine(FXLOG_IT_API_O_SOCKETS) << "it_dto_flags_t   " << (void *) dto_flags << EndLogLine;
  BegLogLine(FXLOG_IT_API_O_SOCKETS) << "it_rdma_addr_t   " << (void *) rdma_addr         << EndLogLine;
  BegLogLine(FXLOG_IT_API_O_SOCKETS) << "it_rmr_context_t " << (void *) rmr_context       << EndLogLine;

  StrongAssertLogLine( local_segments )
    << "it_post_rdma_read(): local_segments is NULL "
    << EndLogLine;
  
  // This effectively makes a "handle" for the work request
  // too bad we don't just give it back to the user ... that would relieve order constraints
  iWARPEM_Object_WorkRequest_t *SendWR =
                  (iWARPEM_Object_WorkRequest_t*) malloc( sizeof(iWARPEM_Object_WorkRequest_t) );

  StrongAssertLogLine( SendWR )
    << "it_post_rdma_read():"
    << " failed to allocate memory for Send work request object "
    << EndLogLine;

  bzero( SendWR, sizeof( iWARPEM_Object_WorkRequest_t ) );

  SendWR->ep_handle      = ep_handle;
  SendWR->num_segments   = num_segments;

  int NumSegSize = sizeof( it_lmr_triplet_t ) * num_segments;
  SendWR->segments_array = (it_lmr_triplet_t *) malloc( NumSegSize );
  
  AssertLogLine( SendWR->segments_array ) << EndLogLine;
  

  memcpy( SendWR->segments_array, local_segments, NumSegSize );

  SendWR->cookie         = cookie;
  SendWR->dto_flags      = dto_flags;

  SendWR->mMessageHdr.mMsg_Type      = iWARPEM_DTO_RDMA_READ_REQ_TYPE;
  SendWR->mMessageHdr.EndianConvert() ;
  SendWR->mMessageHdr.mTotalDataLen  = 0;

  int DataToReadLen = 0;
  for( int i = 0; i < num_segments; i++ )
    {
      AssertLogLine( local_segments[ i ].lmr != NULL )
	<< "it_post_rdma_read(): ERROR:: "
	<< " i: " << i
	<< " num_segments: " << num_segments
	<< EndLogLine;
      
      DataToReadLen += local_segments[ i ].length;
    }

  SendWR->mMessageHdr.mOpType.mRdmaReadReq.mRMRAddr       = rdma_addr;
  SendWR->mMessageHdr.mOpType.mRdmaReadReq.mRMRContext    = rmr_context;
  SendWR->mMessageHdr.mOpType.mRdmaReadReq.mDataToReadLen = htonl(DataToReadLen);
  SendWR->mMessageHdr.mOpType.mRdmaReadReq.mPrivatePtr    = (void *) SendWR;
  
  // Now need to enqueue work order to EndPoint and send if possible
  // gSendWrQueue->Enqueue( SendWR );
  
  //iWARPEM_Object_EndPoint_t* LocalEndPoint = (iWARPEM_Object_EndPoint_t * ) ep_handle;
  //LocalEndPoint->SendWrQueue.Enqueue( SendWR );

  // gSendWrQueue->Enqueue( SendWR );
  iwarpem_enqueue_send_wr( gSendWrQueue, SendWR );

  BegLogLine( FXLOG_IT_API_O_SOCKETS )
    << "it_post_rdma_read(): Enqueued a SendWR  request on: "    
    << " ep_handle: " << *(iWARPEM_Object_EndPoint_t *)ep_handle
    << " SendWR: " << (void *) SendWR
    << EndLogLine;

  pthread_mutex_unlock( & gITAPIFunctionMutex );

  return IT_SUCCESS;
}

it_status_t iwarpem_it_ep_disconnect_resp ( iWARPEM_Object_EndPoint_t* aLocalEndPoint )
{
  iWARPEM_Object_WorkRequest_t *SendWR =
    (iWARPEM_Object_WorkRequest_t*) malloc( sizeof(iWARPEM_Object_WorkRequest_t) );
  
  StrongAssertLogLine( SendWR )
    << "iwarpem_it_ep_disconnect_resp(): ERROR::"
    << " failed to allocate memory for Send work request object "
    << EndLogLine;
  
  bzero( SendWR, sizeof( iWARPEM_Object_WorkRequest_t ) );
  
  SendWR->ep_handle                  = (it_ep_handle_t) aLocalEndPoint;
  SendWR->mMessageHdr.mMsg_Type      = iWARPEM_DISCONNECT_RESP_TYPE;
  SendWR->mMessageHdr.EndianConvert() ;
  SendWR->segments_array             = NULL;

  BegLogLine( FXLOG_IT_API_O_SOCKETS ) 
    << "iwarpem_it_ep_disconnect_resp(): About to enqueue"
    << " SendWR: " << (void *) SendWR
    << EndLogLine;

  // aLocalEndPoint->SendWrQueue.Enqueue( SendWR );    

  iwarpem_enqueue_send_wr( gRecvToSendWrQueue, SendWR );

  // gRecvToSendWrQueue->Enqueue( SendWR );

  BegLogLine( FXLOG_IT_API_O_SOCKETS )
    << "iwarpem_it_ep_disconnect_resp(): Enqueued a SendWR  request on: "    
    << " ep_handle: " <<  *aLocalEndPoint
    << " SendWR: " << (void *) SendWR
    << EndLogLine;
  
  return IT_SUCCESS;
}

it_status_t iwarpem_generate_rdma_read_cmpl_event( iWARPEM_Object_WorkRequest_t * aSendWR )
{
  aSendWR->mMessageHdr.mMsg_Type = iWARPEM_DTO_RDMA_READ_CMPL_TYPE;
  aSendWR->mMessageHdr.EndianConvert() ;

  iwarpem_enqueue_send_wr( gRecvToSendWrQueue, aSendWR );

  return IT_SUCCESS;
}

it_status_t iwarpem_it_post_rdma_read_resp ( 
  IN int                                SocketFd,
  IN it_lmr_triplet_t*                  LocalSegment,
  IN void*                              RdmaReadClientWorkRequestState 
  )
  {
    BegLogLine(FXLOG_IT_API_O_SOCKETS) << "iwarpem_it_post_rdma_read_resp(): " << EndLogLine;
    BegLogLine(FXLOG_IT_API_O_SOCKETS) << "SocketFd:   "      <<  SocketFd << EndLogLine;
    BegLogLine(FXLOG_IT_API_O_SOCKETS) << "it_lmr_triplet_t "
				       << " @ "           << (void*) LocalSegment
				       << "->lmr (handle) " << (void*) LocalSegment->lmr
				       << "->addr->abs "  << (void*) LocalSegment->addr.abs
				       << "->length "     << LocalSegment->length
				       << EndLogLine;
    BegLogLine(FXLOG_IT_API_O_SOCKETS) << "RdmaReadClientWorkRequestState: " << RdmaReadClientWorkRequestState  << EndLogLine;
    
    // This effectively makes a "handle" for the work request
    // too bad we don't just give it back to the user ... that would relieve order constraints
    iWARPEM_Object_WorkRequest_t *SendWR =
      (iWARPEM_Object_WorkRequest_t*) malloc( sizeof(iWARPEM_Object_WorkRequest_t) );
    
    StrongAssertLogLine( SendWR )
      << "iwarpem_it_post_rdma_read_resp: "
      << " failed to allocate memory for Send work request object "
      << EndLogLine;
    
    bzero( SendWR, sizeof( iWARPEM_Object_WorkRequest_t ) );


    iWARPEM_Object_EndPoint_t* LocalEndPoint = gSockFdToEndPointMap[ SocketFd ];
    int num_segments = 1;
    SendWR->ep_handle      = (it_ep_handle_t) LocalEndPoint;
    SendWR->num_segments   = num_segments;

    int NumSegSize = sizeof( it_lmr_triplet_t ) * num_segments;
    SendWR->segments_array = (it_lmr_triplet_t *) malloc( NumSegSize );
    StrongAssertLogLine( SendWR->segments_array ) << EndLogLine;
    memcpy( SendWR->segments_array, LocalSegment, NumSegSize );

    // SendWR->cookie         = 0;
    SendWR->dto_flags      = (it_dto_flags_t) 0;

    SendWR->mMessageHdr.mMsg_Type      = iWARPEM_DTO_RDMA_READ_RESP_TYPE;
    SendWR->mMessageHdr.EndianConvert() ;
    SendWR->mMessageHdr.mTotalDataLen  = 0;

#if IT_API_CHECKSUM
  SendWR->mMessageHdr.mChecksum = 0;
#endif

    for( int i = 0; i < num_segments; i++ )
      {
	AssertLogLine( LocalSegment[ i ].lmr != NULL )
	  << "iwarpem_it_post_rdma_read_resp(): ERROR:: "
	  << " i: " << i
	  << " num_segments: " << num_segments
          << " LocalSegment[ i ].length: " << LocalSegment[ i ].length
	  << EndLogLine;

	AssertLogLine( LocalSegment[ i ].addr.abs != NULL )
	  << "iwarpem_it_post_rdma_read_resp(): ERROR:: LocalSegment[ i ].addr.abs != NULL"
          << " LocalSegment[ i ].length: " << LocalSegment[ i ].length
	  << " i: " << i
	  << " num_segments: " << num_segments
	  << EndLogLine;

#if 0
	AssertLogLine( LocalSegment[ i ].length <= 1024 * 1024 )
	  << "iwarpem_it_post_rdma_read_resp(): ERROR:: "
          << " LocalSegment[ i ].length: " << LocalSegment[ i ].length
	  << EndLogLine;
#endif
	
	SendWR->mMessageHdr.mTotalDataLen += LocalSegment[ i ].length;

#if IT_API_CHECKSUM
      for( int j = 0; j<LocalSegment[ i ].length; j++ )
        {
          SendWR->mMessageHdr.mChecksum += ((char*)SendWR->segments_array[ i ].addr.abs)[ j ];
        }
#endif
      }

    SendWR->mMessageHdr.mOpType.mRdmaReadResp.mPrivatePtr 
      = RdmaReadClientWorkRequestState;

    // Now need to enqueue work order to EndPoint and send if possible
    //gSendWrQueue->Enqueue( SendWR );
    // gRecvToSendWrQueue->Enqueue( SendWR );
    iwarpem_enqueue_send_wr( gRecvToSendWrQueue, SendWR );

    // LocalEndPoint->SendWrQueue.Enqueue( SendWR );    
  
  BegLogLine( FXLOG_IT_API_O_SOCKETS )
    << "iwarpem_it_post_rdma_read_resp(): Enqueued a SendWR request on: " 
    << " ep_handle: " << *LocalEndPoint
    << " SendWR: " << (void *) SendWR
    << EndLogLine;

    return(IT_SUCCESS);
  }

// U it_post_rdma_write
it_status_t it_post_rdma_write (
  IN        it_ep_handle_t    ep_handle,
  IN  const it_lmr_triplet_t *local_segments,
  IN        size_t            num_segments,
  IN        it_dto_cookie_t   cookie,
  IN        it_dto_flags_t    dto_flags,
  IN        it_rdma_addr_t    rdma_addr,
  IN        it_rmr_context_t  rmr_context
  )
  {
  pthread_mutex_lock( & gITAPIFunctionMutex );

  BegLogLine(FXLOG_IT_API_O_SOCKETS) << "it_post_rdma_write(): " << EndLogLine;
  BegLogLine(FXLOG_IT_API_O_SOCKETS) << "it_ep_handle_t   "      << *(iWARPEM_Object_EndPoint_t *)ep_handle << EndLogLine;
  BegLogLine(FXLOG_IT_API_O_SOCKETS) << "it_lmr_triplet_t "
    << " @ "           << (void*) local_segments
    << "->lmr (handle) " << (void*) local_segments->lmr
    << "->addr->abs "  << (void*) local_segments->addr.abs
    << "->length "     << local_segments->length
    << EndLogLine;
  BegLogLine(FXLOG_IT_API_O_SOCKETS) << "size_t           " << num_segments      << EndLogLine;
  // BegLogLine(1) << "it_dto_cookie_t  " << (unsigned long long) cookie << EndLogLine;
  BegLogLine(FXLOG_IT_API_O_SOCKETS) << "it_dto_flags_t   " << (void *) dto_flags << EndLogLine;
  BegLogLine(FXLOG_IT_API_O_SOCKETS) << "it_rdma_addr_t   " << (void *) rdma_addr         << EndLogLine;
  BegLogLine(FXLOG_IT_API_O_SOCKETS) << "it_rmr_context_t " << (void *) rmr_context       << EndLogLine;

  StrongAssertLogLine( local_segments )
    << "it_post_rdma_write(): local_segments is NULL "
    << EndLogLine;

  // This effectively makes a "handle" for the work request
  // too bad we don't just give it back to the user ... that would relieve order constraints
  iWARPEM_Object_WorkRequest_t *SendWR =
                  (iWARPEM_Object_WorkRequest_t*) malloc( sizeof(iWARPEM_Object_WorkRequest_t) );

  StrongAssertLogLine( SendWR )
    << "it_post_rdma_write():"
    << " failed to allocate memory for Send work request object "
    << EndLogLine;

  bzero( SendWR, sizeof( iWARPEM_Object_WorkRequest_t ) );

  SendWR->ep_handle      = ep_handle;
  SendWR->num_segments   = num_segments;

  int NumSegSize = sizeof( it_lmr_triplet_t ) * num_segments;
  SendWR->segments_array = (it_lmr_triplet_t *) malloc( NumSegSize );
  StrongAssertLogLine( SendWR->segments_array ) << EndLogLine;
  memcpy( SendWR->segments_array, local_segments, NumSegSize );

  SendWR->cookie         = cookie;
  SendWR->dto_flags      = dto_flags;

  SendWR->mMessageHdr.mMsg_Type      = iWARPEM_DTO_RDMA_WRITE_TYPE;
  SendWR->mMessageHdr.EndianConvert() ;
  SendWR->mMessageHdr.mTotalDataLen  = 0;

#if IT_API_CHECKSUM
  SendWR->mMessageHdr.mChecksum = 0;
#endif

  for( int i = 0; i < num_segments; i++ )
    {
      AssertLogLine( local_segments[ i ].lmr != NULL )
	<< "it_post_rdma_write(): ERROR:: "
	<< " i: " << i
	<< " num_segments: " << num_segments
	<< EndLogLine;

      SendWR->mMessageHdr.mTotalDataLen += local_segments[ i ].length;
      SendWR->segments_array[i].length=htonl(SendWR->segments_array[i].length);

#if IT_API_CHECKSUM
      for( int j = 0; j<local_segments[ i ].length; j++ )
        {
          SendWR->mMessageHdr.mChecksum += ((char *) SendWR->segments_array[ i ].addr.abs)[ j ];
        }
#endif
    }
  SendWR->mMessageHdr.mTotalDataLen=htonl(SendWR->mMessageHdr.mTotalDataLen) ;

  BegLogLine(FXLOG_IT_API_O_SOCKETS)
    << "Endian-converting rdma_addr=" << (void *) rdma_addr
    << " rmr_context=" << (void *) rmr_context
    << EndLogLine ;
  SendWR->mMessageHdr.mOpType.mRdmaWrite.mRMRAddr = htobe64(rdma_addr);
  SendWR->mMessageHdr.mOpType.mRdmaWrite.mRMRContext = htobe64(rmr_context);

  BegLogLine(FXLOG_IT_API_O_SOCKETS)
    << "SendWR->mMessageHdr.mOpType.mRdmaWrite.mRMRAddr=" << (void *) SendWR->mMessageHdr.mOpType.mRdmaWrite.mRMRAddr
    << " SendWR->mMessageHdr.mOpType.mRdmaWrite.mRMRContext=" << (void *) SendWR->mMessageHdr.mOpType.mRdmaWrite.mRMRContext
    << EndLogLine ;
  int* CookieAsIntPtr = (int *) & SendWR->cookie;

  BegLogLine( 0 )
    << "it_post_rdma_write(): "
    << " SendWR: " << (void *) SendWR
    << " DTO_Type: " << SendWR->mMessageHdr.mMsg_Type
    << " EP: " << *((iWARPEM_Object_EndPoint_t *)SendWR->ep_handle)
    << " cookie: " 
    << FormatString( "%08X" ) << CookieAsIntPtr[ 0 ] 
    << " "
    << FormatString( "%08X" ) << CookieAsIntPtr[ 1 ] 
    << " TotalLen: " << SendWR->mMessageHdr.mTotalDataLen
    << EndLogLine;

  // Now need to enqueue work order to EndPoint and send if possible
  // gSendWrQueue->Enqueue( SendWR );
  iwarpem_enqueue_send_wr( gSendWrQueue, SendWR );

  //iWARPEM_Object_EndPoint_t* LocalEndPoint = (iWARPEM_Object_EndPoint_t * ) ep_handle;
  //LocalEndPoint->SendWrQueue.Enqueue( SendWR );
    
  BegLogLine( FXLOG_IT_API_O_SOCKETS )
    << "it_post_rdma_write(): Enqueued a SendWR request on: " 
    << " ep_handle: " << *(iWARPEM_Object_EndPoint_t *)ep_handle
    << " SendWR: " << (void *) SendWR
    << EndLogLine;

  pthread_mutex_unlock( & gITAPIFunctionMutex );

  return(IT_SUCCESS);
  }

// U it_post_recv
it_status_t it_post_recv (
  IN        it_handle_t       handle,
  IN  const it_lmr_triplet_t *local_segments,
  IN        size_t            num_segments,
  IN        it_dto_cookie_t   cookie,
  IN        it_dto_flags_t    dto_flags
  )
  {
  pthread_mutex_lock( & gITAPIFunctionMutex );

  BegLogLine(FXLOG_IT_API_O_SOCKETS) << "it_post_recv()" << EndLogLine;
  BegLogLine(FXLOG_IT_API_O_SOCKETS) << "it_handle_t (ep or srq?) " << (void*)  handle << EndLogLine;
  BegLogLine(FXLOG_IT_API_O_SOCKETS) << "it_lmr_triplet_t "
    << " @ "           << (void*) local_segments
    << "->lmr (handle) " << (void*) local_segments->lmr
    << "->addr->abs "  << (void*) local_segments->addr.abs
    << "->length "     << local_segments->length
    << EndLogLine;
  BegLogLine(FXLOG_IT_API_O_SOCKETS) << "size_t           " << num_segments     << EndLogLine;
  BegLogLine(FXLOG_IT_API_O_SOCKETS) << "it_dto_cookie_t  { " 
				     << cookie.mFirst << " , "
				     << cookie.mSecond 
				     << " } "
				     << EndLogLine;
  BegLogLine(FXLOG_IT_API_O_SOCKETS) << "it_dto_flags_t   " << (void*)dto_flags << EndLogLine;


  // Enqueue the buffer on the list of available buffers
  // This effectively makes a "handle" for the work request
  // too bad we don't just give it back to the user ... that would relieve order constraints
  iWARPEM_Object_WorkRequest_t *RecvWR =
                  (iWARPEM_Object_WorkRequest_t*) malloc( sizeof(iWARPEM_Object_WorkRequest_t) );

  StrongAssertLogLine( RecvWR )
    << "it_post_recv():"
    << " failed to allocate memory for recv work request object "
    << EndLogLine;

  bzero( RecvWR, sizeof( iWARPEM_Object_WorkRequest_t ) );

  RecvWR->ep_handle      = (it_ep_handle_t) handle;
  RecvWR->num_segments   = num_segments;


  int NumSegSize = sizeof( it_lmr_triplet_t ) * num_segments;
  RecvWR->segments_array = (it_lmr_triplet_t *) malloc( NumSegSize );
  StrongAssertLogLine( RecvWR->segments_array ) << EndLogLine;
  memcpy( RecvWR->segments_array, local_segments, NumSegSize );


  RecvWR->cookie         = cookie;
  RecvWR->dto_flags      = dto_flags;
  
  RecvWR->mMessageHdr.mMsg_Type      = iWARPEM_DTO_RECV_TYPE;
  RecvWR->mMessageHdr.EndianConvert() ;
  RecvWR->mMessageHdr.mTotalDataLen  = 0;
  
  for( int i = 0; i < num_segments; i++ )
    {
      RecvWR->mMessageHdr.mTotalDataLen += local_segments[ i ].length;
    }
  
  int enqrc = ((iWARPEM_Object_EndPoint_t *) handle)->RecvWrQueue.Enqueue( RecvWR );

  AssertLogLine( enqrc == 0 )
    << "it_post_recv(): ERROR:: "
    << " enqrc: " << enqrc
    << EndLogLine;  

  BegLogLine( FXLOG_IT_API_O_SOCKETS )
    << "it_post_recv(): Enqueue a recv request on: "    
    << " ep_handle: " << *(iWARPEM_Object_EndPoint_t *)handle
    << " RecvWR: " << (void *) RecvWR
    << EndLogLine;

  pthread_mutex_unlock( & gITAPIFunctionMutex );

  return( IT_SUCCESS );
  }

// U it_post_send
it_status_t it_post_send (
  IN        it_ep_handle_t    ep_handle,
  IN  const it_lmr_triplet_t *local_segments,
  IN        size_t            num_segments,
  IN        it_dto_cookie_t   cookie,
  IN        it_dto_flags_t    dto_flags
  )
  {
  pthread_mutex_lock( & gITAPIFunctionMutex );

  BegLogLine(FXLOG_IT_API_O_SOCKETS) << "it_post_send()" << EndLogLine;
  BegLogLine(FXLOG_IT_API_O_SOCKETS) << "it_handle_t ep_handle " << *(iWARPEM_Object_EndPoint_t *)ep_handle << EndLogLine;
  BegLogLine(FXLOG_IT_API_O_SOCKETS) << "it_lmr_triplet_t "
    << " @ "           << (void*) local_segments
    << "->lmr (handle) " << (void*) local_segments->lmr
    << "->addr->abs "  << (void*) local_segments->addr.abs
    << "->length "     << local_segments->length
    << EndLogLine;
  BegLogLine(FXLOG_IT_API_O_SOCKETS) << "size_t           " << num_segments     << EndLogLine;
  BegLogLine(FXLOG_IT_API_O_SOCKETS) << "it_dto_cookie_t  { " 
				     << cookie.mFirst << " , "
				     << cookie.mSecond 
				     << " } "
				     << EndLogLine;
  BegLogLine(FXLOG_IT_API_O_SOCKETS) << "it_dto_flags_t   " << (void*)dto_flags << EndLogLine;

  StrongAssertLogLine( local_segments )
    << "it_post_send(): local_segments is NULL "
    << EndLogLine;

  // This effectively makes a "handle" for the work request
  // too bad we don't just give it back to the user ... that would relieve order constraints
  iWARPEM_Object_WorkRequest_t *SendWR =
                  (iWARPEM_Object_WorkRequest_t*) malloc( sizeof(iWARPEM_Object_WorkRequest_t) );

  StrongAssertLogLine( SendWR )
    << "it_post_send():"
    << " failed to allocate memory for Send work request object "
    << EndLogLine;

  bzero( SendWR, sizeof( iWARPEM_Object_WorkRequest_t ) );
  SendWR->ep_handle                  = ep_handle;
  SendWR->num_segments               = num_segments;

  int NumSegSize = sizeof( it_lmr_triplet_t ) * num_segments;
  SendWR->segments_array = (it_lmr_triplet_t *) malloc( NumSegSize );
  StrongAssertLogLine( SendWR->segments_array ) << EndLogLine;
  memcpy( SendWR->segments_array, local_segments, NumSegSize );

  SendWR->cookie         = cookie;
  SendWR->dto_flags      = dto_flags;
  
  SendWR->mMessageHdr.mMsg_Type      = iWARPEM_DTO_SEND_TYPE;
  SendWR->mMessageHdr.EndianConvert() ;
  SendWR->mMessageHdr.mTotalDataLen  = 0;

#if IT_API_CHECKSUM
  SendWR->mMessageHdr.mChecksum = 0;
#endif

  for( int i = 0; i < num_segments; i++ )
    {
      AssertLogLine( local_segments[ i ].lmr != NULL )
	<< "it_post_send(): ERROR:: "
	<< " i: " << i
	<< " num_segments: " << num_segments
	<< EndLogLine;
      
      SendWR->mMessageHdr.mTotalDataLen += local_segments[ i ].length;

#if IT_API_CHECKSUM
      for( int j = 0; j<local_segments[ i ].length; j++ )
        {
          SendWR->mMessageHdr.mChecksum += ((char*)SendWR->segments_array[ i ].addr.abs)[ j ];
        }
#endif
    }
  
  // Now need to enqueue work order to EndPoint and send if possible
  // gSendWrQueue->Enqueue( SendWR );
  iwarpem_enqueue_send_wr( gSendWrQueue, SendWR );

  // iWARPEM_Object_EndPoint_t* LocalEndPoint = (iWARPEM_Object_EndPoint_t * ) ep_handle;
  // LocalEndPoint->SendWrQueue.Enqueue( SendWR );  

  BegLogLine( FXLOG_IT_API_O_SOCKETS )
    << "it_post_send(): Enqueued a SendWR  request on: "    
    << " ep_handle: " << *(iWARPEM_Object_EndPoint_t *)ep_handle
    << " SendWR: " << (void *) SendWR
    << EndLogLine;

  pthread_mutex_unlock( & gITAPIFunctionMutex );

  return(IT_SUCCESS);
  }



/********************************************************************
 * Extended IT_API 
 ********************************************************************/
it_status_t
itx_get_rmr_context_for_ep( IN  it_ep_handle_t    ep_handle,
			   IN  it_lmr_handle_t   lmr,
			   OUT it_rmr_context_t* rmr_context )
{
  *rmr_context = (it_rmr_context_t) lmr;
  
  return IT_SUCCESS;
}

it_status_t
itx_bind_ep_to_device( IN  it_ep_handle_t          ep_handle,
		       IN  it_cn_est_identifier_t  cn_id )
{  
  return IT_SUCCESS;
}

static it_api_o_sockets_aevd_mgr_t* gAEVD ;
static void it_api_o_sockets_signal_accept(void)
  {
    BegLogLine(FXLOG_IT_API_O_SOCKETS_CONNECT)
        << "Signalling the main thread, gAEVD=" << gAEVD
        << EndLogLine ;
    StrongAssertLogLine(gAEVD) << EndLogLine ;
    // Signal the main thread
    pthread_mutex_lock( &gAEVD->mEventCounterMutex );
    (gAEVD->mEventCounter)++;
    pthread_cond_signal( &gAEVD->mMainCond );
    pthread_mutex_unlock( &gAEVD->mEventCounterMutex );

  }
it_status_t
itx_aevd_wait( IN  it_evd_handle_t evd_handle,     
	       IN  uint64_t        timeout,
	       IN  size_t          max_event_count,
	       OUT it_event_t     *events,
	       OUT size_t         *events_count)
{
    it_api_o_sockets_aevd_mgr_t* AEVD = (it_api_o_sockets_aevd_mgr_t *) evd_handle;
    gAEVD = AEVD ;
    BegLogLine(FXLOG_IT_API_O_SOCKETS_LOOP)
        << "gAEVD=" << gAEVD
        << " timeout=" << timeout
        << " max_event_count=" << max_event_count
        << " mutex=" << &( AEVD->mEventCounterMutex )
        << EndLogLine ;

    /************************************************************
     * Block on event ready notification from the processing
     * threads
     ************************************************************/
    pthread_mutex_lock( & ( AEVD->mEventCounterMutex ) );
    BegLogLine(FXLOG_IT_API_O_SOCKETS_LOOP)
      << "Locked the mutex"
      << EndLogLine ;
    if( timeout == 0 )
      {
        if ( AEVD->mEventCounter != 0 )
          {
        BegLogLine(FXLOG_IT_API_O_SOCKETS_CONNECT)
            << "AEVD->mEventCounter=" << AEVD->mEventCounter
            << EndLogLine ;
          }
        // early exit if there are no events yet
        if( AEVD->mEventCounter == 0 )
          {
            // pthread_mutex_unlock( & ( AEVD->mEventCounterMutex ) );
            // *events_count = 0;
            // return IT_SUCCESS;
          }
      }
    else if( (timeout == IT_TIMEOUT_INFINITE) )
      {
        BegLogLine(FXLOG_IT_API_O_SOCKETS_CONNECT)
            << "AEVD->mEventCounter=" << AEVD->mEventCounter
            << EndLogLine ;
        while( AEVD->mEventCounter == 0 )
          {
            BegLogLine(FXLOG_IT_API_O_SOCKETS_CONNECT)
                << "Wait, AEVD=" << AEVD
                << EndLogLine;
            pthread_cond_wait( &(AEVD->mMainCond), &(AEVD->mEventCounterMutex) );
            BegLogLine(FXLOG_IT_API_O_SOCKETS_CONNECT)
                << "Wakeup, mEventCounter=" << AEVD->mEventCounter
                << EndLogLine ;
          }
      }
    else
      {
        // timeout is in milliseconds
        BegLogLine(FXLOG_IT_API_O_SOCKETS_CONNECT)
            << "timeout=" << timeout
            << EndLogLine ;
        struct timespec t;

        t.tv_sec  = ( timeout / 1000 );
        t.tv_nsec = ( timeout - (t.tv_sec*1000) ) * 1000000;

        pthread_cond_timedwait( &(AEVD->mMainCond), &(AEVD->mEventCounterMutex), &t );
        if( AEVD->mEventCounter == 0 )
          {
            *events_count = 0;
            pthread_mutex_unlock( & ( AEVD->mEventCounterMutex ) );
            return IT_SUCCESS;
          }
      }
    pthread_mutex_unlock( & ( AEVD->mEventCounterMutex ) );
    /************************************************************/

    int gatheredEventCount = 0;

//    /***********************************************************************************
//     * Dequeue AFF Events
//     ***********************************************************************************/
    int availableEventSlotsCount = max_event_count;
//    int deviceCount = AEVD->mDevice->devices_count;
    int deviceCount = 1 ;
//    for( int deviceOrd = 0; deviceOrd < deviceCount; deviceOrd++ )
//      {
//        int eventCountInQueue = AEVD->mAffQueues[ deviceOrd ].GetCount();
//        if( eventCountInQueue > 0 )
//          {
//            int eventCount = min( availableEventSlotsCount, eventCountInQueue );
//
//            for( int i = 0; i < eventCount; i++ )
//              {
//                AEVD->mAffQueues[ deviceOrd ].Dequeue( & events[ gatheredEventCount ] );
//                gatheredEventCount++;
//                availableEventSlotsCount--;
//              }
//          }
//      }
//    /***********************************************************************************/
//
//
//
//
//    /***********************************************************************************
//     * Dequeue CM Events
//     ***********************************************************************************/
//    AssertLogLine( availableEventSlotsCount >= 0 )
//      << "ERROR: "
//      << " availableEventSlotsCount: " << availableEventSlotsCount
//      << EndLogLine;
//
//    int eventCountInCMQueue = AEVD->mCMQueue.GetCount();
//    BegLogLine(FXLOG_IT_API_O_SOCKETS_CONNECT)
//      << "&AEVD->mCMQueue=" << &AEVD->mCMQueue
//      << " eventCountInCMQueue=" << eventCountInCMQueue
//      << EndLogLine ;
//    if( eventCountInCMQueue > 0 )
//      {
//        int eventCount = min( availableEventSlotsCount, eventCountInCMQueue );
//
//        for( int i = 0; i < eventCount; i++ )
//          {
//            AEVD->mCMQueue.Dequeue( & events[ gatheredEventCount ] );
//            gatheredEventCount++;
//            availableEventSlotsCount--;
//          }
//      }
//    /***********************************************************************************/
//    static unsigned long loopCount ;
//    loopCount += 1 ;
    /*
     * Dequeue CMM events
     */
    BegLogLine(FXLOG_IT_API_O_SOCKETS_LOOP )
      << "AEVD->mCMQueue=" << AEVD->mCMMEVQObj
      << EndLogLine ;
//    iWARPEM_Object_EventQueue_t* CMMEVQObj = (iWARPEM_Object_EventQueue_t *) evd_cmm_handle;
    iWARPEM_Object_EventQueue_t* CMMEVQObj = AEVD->mCMMEVQObj;

    iWARPEM_Object_Event_t *EventPtr;

//    int rc = EVQObj->Dequeue( &EventPtr );
//
//    if( rc == 0 )
//      {
//        AssertLogLine( EventPtr != NULL )
//          << "it_evd_dequeue(): ERROR: EventPtr is NULL"
//          << " EVQObj: " << (void *) EVQObj
//          << " EVQObj->Queue: " << (void *) &(EVQObj->mQueue)
//          << EndLogLine;
//
//      *event = EventPtr->mEvent;
//
//      BegLogLine( FXLOG_IT_API_O_SOCKETS )
//        << "About to call free( " << (void *) EventPtr << " )"
//        << EndLogLine;
//
//      free( EventPtr );
        int rc = CMMEVQObj->Dequeue( & EventPtr );
        int eventCountInCMMQueue = ( rc == 0 ) ? 1 : 0 ;
        if ( eventCountInCMMQueue != 0 )
          {
            BegLogLine(FXLOG_IT_API_O_SOCKETS )
              << "AEVD->mCMMEVQObj=" << AEVD->mCMMEVQObj
              << EndLogLine ;
        BegLogLine(FXLOG_IT_API_O_SOCKETS_CONNECT)
          << " eventCountInCMMQueue=" << eventCountInCMMQueue
          << EndLogLine ;
          }
        if( eventCountInCMMQueue > 0 )
          {
            int eventCount = min( availableEventSlotsCount, eventCountInCMMQueue );

//            for( int i = 0; i < eventCount; i++ )
//              {
//                CMMEVQObj->Dequeue( & EventPtr );
                events[ gatheredEventCount ] = EventPtr->mEvent;
                BegLogLine(FXLOG_IT_API_O_SOCKETS_CONNECT)
                  << "events[" << gatheredEventCount
                  << "].event_number=" << events[gatheredEventCount].event_number
                  << EndLogLine ;
                BegLogLine( FXLOG_IT_API_O_SOCKETS )
                  << "About to call free( " << (void *) EventPtr << " )"
                  << EndLogLine;
                free( EventPtr );

                gatheredEventCount++;
                availableEventSlotsCount--;
//              }
                pthread_mutex_lock( & ( AEVD->mEventCounterMutex ) );
                AEVD->mEventCounter += eventCountInCMMQueue;
                pthread_mutex_unlock( & ( AEVD->mEventCounterMutex ) );
          }

    /*
     * Dequeue CM events
     */
      BegLogLine(FXLOG_IT_API_O_SOCKETS_LOOP )
        << "CMQueue=" << CMQueue
        << EndLogLine ;
          int eventCountInCMQueue = CMQueue->GetCount();
          if ( eventCountInCMQueue != 0 )
            {
          BegLogLine(FXLOG_IT_API_O_SOCKETS_CONNECT)
            << " eventCountInCMQueue=" << eventCountInCMQueue
            << EndLogLine ;
            }
          if( eventCountInCMQueue > 0 )
            {
              int eventCount = min( availableEventSlotsCount, eventCountInCMQueue );

              for( int i = 0; i < eventCount; i++ )
                {
                  CMQueue->Dequeue( & events[ gatheredEventCount ] );
                  BegLogLine(FXLOG_IT_API_O_SOCKETS_CONNECT)
                    << "events[" << gatheredEventCount
                    << "].event_number=" << events[gatheredEventCount].event_number
                    << EndLogLine ;
                  gatheredEventCount++;
                  availableEventSlotsCount--;
                }
            }




    /***********************************************************************************
     * Dequeue Send CQ Events
     ***********************************************************************************/
    AssertLogLine( availableEventSlotsCount >= 0 )
      << "ERROR: "
      << " availableEventSlotsCount: " << availableEventSlotsCount
      << EndLogLine;

    for( int deviceOrd = 0; deviceOrd < deviceCount; deviceOrd++ )
      {
        BegLogLine(FXLOG_IT_API_O_SOCKETS_LOOP )
          << "&AEVD->mSendQueues[" << deviceOrd
          << "]=" << &AEVD->mSendQueues[ deviceOrd ]
          << EndLogLine ;
        int eventCountInQueue = AEVD->mSendQueues[ deviceOrd ].GetCount();
        if( eventCountInQueue > 0 )
          {
            int eventCount = min( availableEventSlotsCount, eventCountInQueue );

            BegLogLine( FXLOG_IT_API_O_SOCKETS_QUEUE_LENGTHS_LOG )
              << "itx_aevd_wait():: send events: " << eventCount
              << EndLogLine;

            for( int i = 0; i < eventCount; i++ )
              {
                it_event_t* ievent = & events[ gatheredEventCount ];

                AEVD->mSendQueues[ deviceOrd ].Dequeue( ievent );
                availableEventSlotsCount--;
                gatheredEventCount++;

                switch( ievent->event_number )
                  {
                  case IT_DTO_RDMA_READ_CMPL_EVENT:
                    {
//                      gITAPI_RDMA_READ_AT_WAIT.HitOE( IT_API_TRACE,
//                                                      gITAPI_RDMA_READ_AT_WAIT_Name,
//                                                      gTraceRank,
//                                                      gITAPI_RDMA_READ_AT_WAIT );
                      break;
                    }
                  case IT_DTO_RDMA_WRITE_CMPL_EVENT:
                    {
//                      gITAPI_RDMA_WRITE_AT_WAIT.HitOE( IT_API_TRACE,
//                                                       gITAPI_RDMA_WRITE_AT_WAIT_Name,
//                                                       gTraceRank,
//                                                       gITAPI_RDMA_WRITE_AT_WAIT );
                      break;
                    }
                  case IT_DTO_SEND_CMPL_EVENT:
                    {
//                      gITAPI_SEND_AT_WAIT.HitOE( IT_API_TRACE,
//                                                 gITAPI_SEND_AT_WAIT_Name,
//                                                 gTraceRank,
//                                                 gITAPI_SEND_AT_WAIT );
                      break;
                    }
                  default:
                    {
                      StrongAssertLogLine( 0 )
                        << "ERROR: "
                        << " ievent->event_number: " << ievent->event_number
                        << EndLogLine;
                    }
                  }
              }
          }
      }
    /************************************************************************************/




    /***********************************************************************************
     * Dequeue Recv CQ Events
     ***********************************************************************************/
    AssertLogLine( availableEventSlotsCount >= 0 )
      << "ERROR: "
      << " availableEventSlotsCount: " << availableEventSlotsCount
      << EndLogLine;

    for( int deviceOrd = 0; deviceOrd < deviceCount; deviceOrd++ )
      {
        BegLogLine(FXLOG_IT_API_O_SOCKETS_LOOP )
          << "&AEVD->mRecvQueues[" << deviceOrd
          << "]=" << &AEVD->mRecvQueues[ deviceOrd ]
          << EndLogLine ;
        int eventCountInQueue = AEVD->mRecvQueues[ deviceOrd ].GetCount();
        if( eventCountInQueue > 0 )
          {
            int eventCount = min( availableEventSlotsCount, eventCountInQueue );

            BegLogLine( FXLOG_IT_API_O_SOCKETS_QUEUE_LENGTHS_LOG )
              << "itx_aevd_wait():: recv events: " << eventCount
              << EndLogLine;

            for( int i = 0; i < eventCount; i++ )
              {
                AEVD->mRecvQueues[ deviceOrd ].Dequeue( & events[ gatheredEventCount ] );
                gatheredEventCount++;
                availableEventSlotsCount--;

//                gITAPI_RECV_AT_WAIT.HitOE( IT_API_TRACE,
//                                           gITAPI_RECV_AT_WAIT_Name,
//                                           gTraceRank,
//                                           gITAPI_RECV_AT_WAIT );
              }
          }
      }
    /************************************************************************************/

    AssertLogLine( availableEventSlotsCount >= 0 )
      << "ERROR: "
      << " availableEventSlotsCount: " << availableEventSlotsCount
      << EndLogLine;

    AssertLogLine( gatheredEventCount >= 0 )
      << "ERROR: "
      << " gatheredEventCount: " << gatheredEventCount
      << EndLogLine;

    BegLogLine(FXLOG_IT_API_O_SOCKETS_LOOP)
      << "event_number=" << events->event_number
      << EndLogLine ;

    pthread_mutex_lock( & ( AEVD->mEventCounterMutex ) );
    AEVD->mEventCounter -= gatheredEventCount;
    pthread_mutex_unlock( & ( AEVD->mEventCounterMutex ) );

    *events_count = gatheredEventCount;

    return IT_SUCCESS;
//  return IT_ERR_QUEUE_EMPTY;
}
// Extended Accept that allows to transmit an RMR context as private data
// - registers the lmr with the newly created qp
// - makes this info part of the private data and accepts the connection
it_status_t itx_ep_accept_with_rmr (
                                    IN        it_ep_handle_t         ep_handle,
                                    IN        it_cn_est_identifier_t cn_est_id,
                                    IN        it_lmr_triplet_t      *lmr,
                                    OUT       it_rmr_context_t      *rmr_context )
{
  const unsigned char         *private_data = (const unsigned char *)"1";
  size_t                 private_data_length = 1;

    // pthread_mutex_lock( & gITAPIFunctionMutex );

  // this is the moment when the ConReq is first associated with the passive side endpoint
  BegLogLine(FXLOG_IT_API_O_SOCKETS) << "it_ep_accept()" << EndLogLine;
  BegLogLine(FXLOG_IT_API_O_SOCKETS) << "it_ep_handle_t         ep_handle           " << *(iWARPEM_Object_EndPoint_t *)ep_handle   << EndLogLine;
  BegLogLine(FXLOG_IT_API_O_SOCKETS) << "it_cn_est_identifier_t cn_est_id           " << cn_est_id           << EndLogLine;
  BegLogLine(FXLOG_IT_API_O_SOCKETS) << "unsigned char         *private_data @      " << (void*)private_data << EndLogLine;
  BegLogLine(FXLOG_IT_API_O_SOCKETS) << "size_t                 private_data_length " << private_data_length << EndLogLine;

  BegLogLine(FXLOG_IT_API_O_SOCKETS) << "lmr->lmr=" << lmr->lmr
                                     << " lmr->addr.abs=" << lmr->addr.abs
                                     << " lmr->length=" << lmr->length
                                     << EndLogLine ;
  // Use the cn_est_id to get back to information about this connection request
  iWARPEM_Object_ConReqInfo_t* ConReqInfoPtr = (iWARPEM_Object_ConReqInfo_t*) cn_est_id;

  int s=ConReqInfoPtr->ConnFd ;
  BegLogLine(FXLOG_IT_API_O_SOCKETS)
    << "Operating on socket FD=" << s
    << EndLogLine ;
  // Actually should check magic and other values too
//   ConReqInfoPtr only required for its ConnFd, which in the sockets implementation
//   is handled by the underlying accept support
//  StrongAssertLogLine( ConReqInfoPtr != 0 ) << "it_ep_accept(): cn_est_id let to a NULL ConReqInfo object" << EndLogLine;

  // ConReqInfoPtr->AcceptingEndPoint  = ep_handle;
  // ConReqInfoPtr->AcceptRejectStatus = 1; // 1 is accepted

  // fill out our ep stuff
  iWARPEM_Object_EndPoint_t* LocalEndPoint = (iWARPEM_Object_EndPoint_t*) ep_handle;

  StrongAssertLogLine( LocalEndPoint != 0 )
    << "it_ep_accept(): local endpoint handle null"
    << EndLogLine;

  StrongAssertLogLine( LocalEndPoint->ConnectedFlag == IWARPEM_CONNECTION_FLAG_DISCONNECTED )
    << "it_ep_accept(): local endpoint already connected"
    << EndLogLine;

  LocalEndPoint->ConnectedFlag = IWARPEM_CONNECTION_FLAG_CONNECTED;
  LocalEndPoint->ConnFd        = ConReqInfoPtr->ConnFd;

  StrongAssertLogLine( private_data_length > 0 )
    << "it_ep_accept(): ERROR: private_data must be set to the other EP Node Id (needed for debugging)"
    << EndLogLine;

  sscanf( (const char *) private_data, "%d", &(LocalEndPoint->OtherEPNodeId) );   

  unsigned char* internal_private_data = NULL;

  // if lmr and rmr_context are provided:
  // - register rmr with QP
  // - make rmr private data
  iWARPEM_Private_Data_t PrivateData;
  PrivateData.mLen = private_data_length;

  StrongAssertLogLine( PrivateData.mLen < IT_MAX_PRIV_DATA )
    << "itx_ep_accept_with_rmr(): Maximum private data length exceeded. Limit is: " << IT_MAX_PRIV_DATA
    << " actual is: " << PrivateData.mLen
    << EndLogLine;

  if( lmr && rmr_context )
    {
      it_status_t istatus = itx_get_rmr_context_for_ep( ep_handle,
                                           lmr->lmr,
                                           rmr_context );

      if( istatus != IT_SUCCESS )
        {
          return istatus;
        }

      // we have to extend the private data buffer if user provided data already
      unsigned char *transfer_rmr;

      PrivateData.mLen = private_data_length + 3 * sizeof(uint64_t) ; // works also if no user priv-data is present
      StrongAssertLogLine( PrivateData.mLen < IT_MAX_PRIV_DATA )
        << "itx_ep_accept_with_rmr(): Maximum private data length exceeded after adding rmr. max: " << IT_MAX_PRIV_DATA
  << "actual is: " << PrivateData.mLen
        << EndLogLine;

//      internal_private_data = (unsigned char*)malloc( internal_private_data_length );

      if( private_data != NULL )
        {
          memcpy( PrivateData.mData+3 * sizeof(uint64_t) , private_data, private_data_length );
//          transfer_rmr = (unsigned char*) ( &PrivateData.mData[ private_data_length ] );
        }
//      else
//        {
//          transfer_rmr = PrivateData.mData;
//        }
      transfer_rmr = PrivateData.mData;

      *((uint64_t*)&transfer_rmr[ 0 ])                   = htobe64( (uint64_t) (*rmr_context) );
      *((uint64_t*)&transfer_rmr[ sizeof(uint64_t) ])    = htobe64( (uint64_t) (lmr->addr.abs) );
      *((uint64_t*)&transfer_rmr[ sizeof(uint64_t) * 2]) = htobe64( (uint64_t) lmr->length );
      BegLogLine(FXLOG_IT_API_O_SOCKETS)
       << "transfer_rmr=" << HexDump(transfer_rmr,sizeof(uint64_t)*3)
       << EndLogLine ;

    }
  else
    {
      PrivateData.mLen = private_data_length; // assume no data
      memcpy( PrivateData.mData,
      private_data,
      private_data_length);
    }

  int wLen;
  int SizeToSend = sizeof( iWARPEM_Private_Data_t );
  iWARPEM_Status_t wstat = write_to_socket( s,
              (char *) & PrivateData,
              SizeToSend,
              & wLen );
  AssertLogLine( wLen == SizeToSend )
    << "it_ep_accept(): ERROR: "
    << " wLen: " << wLen
    << " SizeToSend: " << SizeToSend
    << EndLogLine;
  BegLogLine(FXLOG_IT_API_O_SOCKETS)
    << "it_ep_accept(): ep " << *(iWARPEM_Object_EndPoint_t *)ep_handle
    << " ConReqInfo@ " << (void*) ConReqInfoPtr
    << " SocketFd: " << ConReqInfoPtr->ConnFd
    << EndLogLine;

  // Add the socket descriptor to the data receiver controller
  iwarpem_add_socket_to_list( ConReqInfoPtr->ConnFd, LocalEndPoint );

  // create an event to to pass to the user
  /* Todo: Seems leaked */
  iWARPEM_Object_Event_t* ConnEstablishedEvent = (iWARPEM_Object_Event_t*) malloc( sizeof( iWARPEM_Object_Event_t ) );

  BegLogLine(FXLOG_IT_API_O_SOCKETS)
    << "ConnEstablishedEvent malloc -> " << (void *) ConnEstablishedEvent
    << EndLogLine ;
  it_connection_event_t* ice = (it_connection_event_t*) & ConnEstablishedEvent->mEvent;  
  
  ice->event_number = IT_CM_MSG_CONN_ESTABLISHED_EVENT;
  ice->evd          = LocalEndPoint->connect_sevd_handle; // i guess?
  ice->cn_est_id    = (it_cn_est_identifier_t) cn_est_id;
  ice->ep           = (it_ep_handle_t) LocalEndPoint;
  
  iWARPEM_Object_EventQueue_t* ConnectEventQueuePtr = 
    (iWARPEM_Object_EventQueue_t*) LocalEndPoint->connect_sevd_handle;

  BegLogLine(FXLOG_IT_API_O_SOCKETS)
    << "ConnectEventQueuePtr = " << ConnectEventQueuePtr
    << EndLogLine ;
// Don't seem to have a ConnectEventQueuePtr->
  int enqrc = ConnectEventQueuePtr->Enqueue( ConnEstablishedEvent );

  StrongAssertLogLine( enqrc == 0 ) << "failed to enqueue connection request event" << EndLogLine;
  
//  free(ConnEstablishedEvent) ; /* TODO check if this plugs a leak */

  BegLogLine(FXLOG_IT_API_O_SOCKETS)
    << "it_ep_accept(): "
    << " ConReqInfo@ "    << (void*) ConReqInfoPtr
    << EndLogLine;     

  free(ConReqInfoPtr) ; /* TODO check if this plugs a leak */

  BegLogLine(FXLOG_IT_API_O_SOCKETS) << "*rmr_context=" << *rmr_context
                                     << EndLogLine ;
  
  //  pthread_mutex_unlock( & gITAPIFunctionMutex );

  return(IT_SUCCESS);
  }

it_status_t it_prepare_connection(
                                  IN        it_api_o_sockets_qp_mgr_t  *qpMgr,
                                  IN  const it_path_t                *path,
                                  IN  const it_conn_attributes_t     *conn_attr,
                                  IN  const it_conn_qual_t           *connect_qual,
                                  IN        it_cn_est_flags_t         cn_est_flags
                                  )
{
  int ret = -1;
  int s;

  if((s = socket(IT_API_SOCKET_FAMILY, SOCK_STREAM, 0)) < 0)
    {
    perror("it_ep_connect socket() open");
    }

#ifdef IT_API_OVER_UNIX_DOMAIN_SOCKETS
  struct sockaddr_un serv_addr;

  memset( &serv_addr, 0, sizeof( serv_addr ) );
  serv_addr.sun_family      = IT_API_SOCKET_FAMILY;

  sprintf( serv_addr.sun_path,
           "%s.%d",
           IT_API_UNIX_SOCKET_PREFIX_PATH,
           connect_qual->conn_qual.lr_port.remote );

  socklen_t serv_addr_len = sizeof( serv_addr.sun_family ) + strlen( serv_addr.sun_path );
#else
  struct sockaddr_in serv_addr;

  memset( &serv_addr, 0, sizeof( serv_addr ) );
  serv_addr.sin_family      = IT_API_SOCKET_FAMILY;
  serv_addr.sin_port        = connect_qual->conn_qual.lr_port.remote;
  serv_addr.sin_addr.s_addr = path->u.iwarp.raddr.ipv4.s_addr;

  socklen_t serv_addr_len = sizeof( serv_addr );
#endif

  int SockSendBuffSize = -1;
  int SockRecvBuffSize = -1;
  //BGF size_t ArgSize = sizeof( int );
  socklen_t ArgSize = sizeof( int );
  getsockopt( s, SOL_SOCKET, SO_SNDBUF, (int *) & SockSendBuffSize, & ArgSize );
  getsockopt( s, SOL_SOCKET, SO_RCVBUF, (int *) & SockRecvBuffSize, & ArgSize );

}
// U it_ep_connect

// An object is needed to hold state between
// the call to connect and the generation of
// a connection event in response to this call.

it_status_t itx_ep_connect_with_rmr (
                                     IN        it_ep_handle_t        ep_handle,
                                     IN  const it_path_t            *path,
                                     IN  const it_conn_attributes_t *conn_attr,
                                     IN  const it_conn_qual_t       *connect_qual,
                                     IN        it_cn_est_flags_t     cn_est_flags,
                                     IN  const unsigned char        *private_data,
                                     IN        size_t                private_data_length,
                                     IN        it_lmr_triplet_t     *lmr,
                                     OUT       it_rmr_context_t     *rmr_context
                                     )
{
  pthread_mutex_lock( & gITAPIFunctionMutex );

  // BegLogLine(FXLOG_IT_API_O_SOCKETS) << "it_ep_connect() entering" << EndLogLine;
  BegLogLine( FXLOG_IT_API_O_SOCKETS_CONNECT ) << "it_ep_connect() entering" << EndLogLine;

  BegLogLine(FXLOG_IT_API_O_SOCKETS) << "IN        it_ep_handle_t        ep_handle,                 " <<  (void *)ep_handle           << EndLogLine;
  BegLogLine(FXLOG_IT_API_O_SOCKETS) << "IN  const it_path_t             path..>raddr.ipv4.s_addr   " <<  (void*) path->u.iwarp.raddr.ipv4.s_addr        << EndLogLine;
  BegLogLine(FXLOG_IT_API_O_SOCKETS) << "IN  const it_conn_attributes_t* conn_attr@                 " <<  (void*) conn_attr                              << EndLogLine;
  BegLogLine(FXLOG_IT_API_O_SOCKETS) << "IN  const it_conn_qual_t        connect_qual..>port.local  " <<  (void*) connect_qual->conn_qual.lr_port.local  << EndLogLine;
  BegLogLine(FXLOG_IT_API_O_SOCKETS) << "IN  const it_conn_qual_t        connect_qual..>port.remote " <<  (void*) connect_qual->conn_qual.lr_port.remote << EndLogLine;
  BegLogLine(FXLOG_IT_API_O_SOCKETS) << "IN        it_cn_est_flags_t     cn_est_flags,              " <<  (void*)  cn_est_flags        << EndLogLine;
  BegLogLine(FXLOG_IT_API_O_SOCKETS) << "IN  const unsigned char         private_data@              " <<  (void*)private_data        << EndLogLine;
  BegLogLine(FXLOG_IT_API_O_SOCKETS) << "IN        size_t                private_data_length        " <<   private_data_length << EndLogLine;

  if( private_data_length > IT_MAX_PRIV_DATA )
    {
      pthread_mutex_unlock( & gITAPIFunctionMutex );

      return IT_ERR_INVALID_PDATA_LENGTH;
    }

  iWARPEM_Object_EndPoint_t* LocalEndPoint = (iWARPEM_Object_EndPoint_t*) ep_handle;

  StrongAssertLogLine( LocalEndPoint != 0 )
    << "it_ep_connect(): local endpoint handle null"
    << EndLogLine;

  StrongAssertLogLine( LocalEndPoint->ConnectedFlag == IWARPEM_CONNECTION_FLAG_DISCONNECTED )
    << "it_ep_connect(): local endpoint already connected"
    << EndLogLine;

  int s;

  if((s = socket(IT_API_SOCKET_FAMILY, SOCK_STREAM, 0)) < 0)
    {
    perror("it_ep_connect socket() open");
    }

#ifdef IT_API_OVER_UNIX_DOMAIN_SOCKETS
  struct sockaddr_un serv_addr;

  memset( &serv_addr, 0, sizeof( serv_addr ) );
  serv_addr.sun_family      = IT_API_SOCKET_FAMILY;

  sprintf( serv_addr.sun_path,
           "%s.%d",
           IT_API_UNIX_SOCKET_PREFIX_PATH,
           connect_qual->conn_qual.lr_port.remote );

  socklen_t serv_addr_len = sizeof( serv_addr.sun_family ) + strlen( serv_addr.sun_path );
#else
  struct sockaddr_in serv_addr;

  memset( &serv_addr, 0, sizeof( serv_addr ) );
  serv_addr.sin_family      = IT_API_SOCKET_FAMILY;
  serv_addr.sin_port        = connect_qual->conn_qual.lr_port.remote;
  serv_addr.sin_addr.s_addr = path->u.iwarp.raddr.ipv4.s_addr;

  socklen_t serv_addr_len = sizeof( serv_addr );
#endif

  int SockSendBuffSize = -1;
  int SockRecvBuffSize = -1;
  //BGF size_t ArgSize = sizeof( int );
  socklen_t ArgSize = sizeof( int );
  getsockopt( s, SOL_SOCKET, SO_SNDBUF, (int *) & SockSendBuffSize, & ArgSize );
  getsockopt( s, SOL_SOCKET, SO_RCVBUF, (int *) & SockRecvBuffSize, & ArgSize );

  BegLogLine( 0 )
    << "it_ep_connect(): "
    << " SockSendBuffSize: " << SockSendBuffSize
    << " SockRecvBuffSize: " << SockRecvBuffSize
    << EndLogLine;

  SockSendBuffSize = IT_API_SOCKET_BUFF_SIZE;
  SockRecvBuffSize = IT_API_SOCKET_BUFF_SIZE;
  setsockopt( s, SOL_SOCKET, SO_SNDBUF, (const char *) & SockSendBuffSize, ArgSize );
  setsockopt( s, SOL_SOCKET, SO_RCVBUF, (const char *) & SockRecvBuffSize, ArgSize );

  while( 1 )
    {
      int ConnRc = connect( s,
          (struct sockaddr *) & serv_addr,
          serv_addr_len );

      if( ConnRc < 0 )
  {
  if( errno != EAGAIN )
    {
      perror("Connection failed") ;
      StrongAssertLogLine( 0 )
        << "it_ep_connect:: Connection failed "
        << " errno: " << errno
        << EndLogLine;
    }
  }
      else if( ConnRc == 0 )
  break;
    }
#if defined(SPINNING_RECEIVE)
  socket_nonblock_on(s) ;
#endif
  socket_nodelay_on(s) ;

  unsigned char* internal_private_data = NULL;

  // if lmr and rmr_context are provided:
  // - register rmr with QP
  // - make rmr private data
  iWARPEM_Private_Data_t PrivateData;
  PrivateData.mLen = private_data_length;

  StrongAssertLogLine( PrivateData.mLen < IT_MAX_PRIV_DATA )
    << "itx_ep_connect_with_rmr(): Maximum private data length exceeded. Limit is: " << IT_MAX_PRIV_DATA
    << " actual is: " << PrivateData.mLen
    << EndLogLine;

  if( lmr && rmr_context )
    {
      it_status_t istatus = itx_get_rmr_context_for_ep( ep_handle,
                                           lmr->lmr,
                                           rmr_context );

      if( istatus != IT_SUCCESS )
        {
          return istatus;
        }

      // we have to extend the private data buffer if user provided data already
      unsigned char *transfer_rmr;

      PrivateData.mLen = private_data_length + 3 * sizeof(uint64_t) ; // works also if no user priv-data is present
      StrongAssertLogLine( PrivateData.mLen < IT_MAX_PRIV_DATA )
        << "itx_ep_connect_with_rmr(): Maximum private data length exceeded after adding rmr. max: " << IT_MAX_PRIV_DATA
	<< "actual is: " << PrivateData.mLen
        << EndLogLine;

//      internal_private_data = (unsigned char*)malloc( internal_private_data_length );

      if( private_data != NULL )
        {
          memcpy( PrivateData.mData, private_data, private_data_length );
          transfer_rmr = (unsigned char*) ( &PrivateData.mData[ private_data_length ] );
        }
      else
        {
          transfer_rmr = PrivateData.mData;
        }
//      it_rmr_triplet_t rmr;
//      rmr.length   = (it_length_t)        ntohl( *(uint32_t*)&( ((const char*)ConnReqEvent->private_data) [sizeof(uint32_t) * 3]) );
//      rmr.rmr      = (it_rmr_handle_t)  be64toh( *(uint64_t*)&( ((const char*)ConnReqEvent->private_data) [sizeof(uint32_t) * 4]) );
//      rmr.addr.abs = (void*)            be64toh( *(uint64_t*)&( ((const char*)ConnReqEvent->private_data) [sizeof(uint32_t) * 4 + sizeof(uint64_t)]) );

      *((uint32_t*)&transfer_rmr[ 0 ])                   = htonl  ( (uint32_t) lmr->length );
      *((uint64_t*)&transfer_rmr[ sizeof(uint32_t) ])    = htobe64( (uint64_t) (*rmr_context) );
      *((uint64_t*)&transfer_rmr[ sizeof(uint32_t) * 3]) = htobe64( (uint64_t) (lmr->addr.abs) );

      BegLogLine(FXLOG_IT_API_O_SOCKETS)
        << "PrivateData sent " << HexDump(PrivateData.mData,sizeof( iWARPEM_Private_Data_t ))
        << EndLogLine ;
    }
  else
    {
      PrivateData.mLen = private_data_length; // assume no data
      memcpy( PrivateData.mData,
	    private_data,
	    private_data_length);
    }

  int wLen;
  int SizeToSend = sizeof( iWARPEM_Private_Data_t );
  iWARPEM_Status_t wstat = write_to_socket( s,
              (char *) & PrivateData,
              SizeToSend,
              & wLen );
  AssertLogLine( wLen == SizeToSend )
    << "it_ep_connect(): ERROR: "
    << " wLen: " << wLen
    << " SizeToSend: " << SizeToSend
    << EndLogLine;

  LocalEndPoint->ConnectedFlag = IWARPEM_CONNECTION_FLAG_CONNECTED;
  LocalEndPoint->ConnFd        = s;

  StrongAssertLogLine( private_data_length > 0 )
    << "it_ep_connect(): ERROR: private_data must be set to the other EP Node Id (needed for debugging)"
    << EndLogLine;

  int Dummy;
  sscanf( (const char *) private_data, "%d %d", &Dummy, &(LocalEndPoint->OtherEPNodeId) );

  iWARPEM_Private_Data_t PrivateDataIn;
  int rlen ;
  read_from_socket(s, (char *)&PrivateDataIn, sizeof(PrivateDataIn),&rlen ) ;
  BegLogLine(FXLOG_IT_API_O_SOCKETS)
    << "PrivateDataIn.mLen=" << PrivateDataIn.mLen
    << EndLogLine ;
  // Add the socket descriptor to the data receiver controller
  iwarpem_add_socket_to_list( s, LocalEndPoint );
  {
    iWARPEM_Object_EndPoint_t* LocalEndPoint = gSockFdToEndPointMap[ s ];


    // Generate the connection established event
    iWARPEM_Object_Event_t* ConnEstablishedEvent = (iWARPEM_Object_Event_t*) malloc( sizeof( iWARPEM_Object_Event_t ) );

    it_connection_event_t* ice = (it_connection_event_t*) & ConnEstablishedEvent->mEvent;
//    iWARPEM_Object_Event_t* CompletetionEvent =
//      (iWARPEM_Object_Event_t*) malloc( sizeof( iWARPEM_Object_Event_t ) );

//    it_connection_event_t* conne = (it_connection_event_t *) & CompletetionEvent->mEvent;
//
//    if( LocalEndPoint->ConnectedFlag == IWARPEM_CONNECTION_FLAG_PASSIVE_SIDE_PENDING_DISCONNECT )
//      conne->event_number = IT_CM_MSG_CONN_DISCONNECT_EVENT;
//    else
//      conne->event_number = IT_CM_MSG_CONN_BROKEN_EVENT;
    ice->event_number   = IT_CM_MSG_CONN_ESTABLISHED_EVENT;

    ice->evd          = LocalEndPoint->connect_sevd_handle;
    ice->ep           = (it_ep_handle_t) LocalEndPoint;
    ice->private_data_present = IT_TRUE ;
    memcpy(ice->private_data,PrivateDataIn.mData,IT_MAX_PRIV_DATA) ;

    iWARPEM_Object_EventQueue_t* ConnCmplEventQueue =
      (iWARPEM_Object_EventQueue_t*) ice->evd;

    int ret = ConnCmplEventQueue->Enqueue( ConnEstablishedEvent );

    BegLogLine(FXLOG_IT_API_O_SOCKETS)
      << "ConnCmplEventQueue=" << ConnCmplEventQueue
      << " ice->event_number=" << ice->event_number
      << " ice->evd=" << ice->evd
      << " ice->ep=" << ice->ep
      << " ice->private_data=" << HexDump(ice->private_data,sizeof( iWARPEM_Private_Data_t ))
      << EndLogLine ;
//    int ret = 0;
    if (ret)
      {
        BegLogLine( 1 )
          << "it_ep_connect(): ERROR: failed to connect to remote host"
          << " ret: " << ret
          << " errno: " << errno
        //  << " conn_id: " << (void*)cm_conn_id
        //  << " conn_param.pdlen: " << conn_param.private_data_len
          << EndLogLine;

        if( internal_private_data )
          free( internal_private_data );

        pthread_mutex_unlock( & gITAPIFunctionMutex );
        return IT_ERR_ABORT;
      }
  }

  // BegLogLine(FXLOG_IT_API_O_SOCKETS)
  BegLogLine( FXLOG_IT_API_O_SOCKETS_CONNECT )
    << "it_ep_connect(): QP connected"
    << EndLogLine;

  /* should be save to free here:
   * [manpage of rdma_connect(): private_data
   *          References a user-controlled data buffer.  The contents
   *          of the buffer are copied and transparently passed to the
   *          remote side as part of the communication request.  May
   *          be NULL if pri- vate_data is not required.
   */
  if( internal_private_data )
    free( internal_private_data );

  pthread_mutex_unlock( & gITAPIFunctionMutex );

  return(IT_SUCCESS);
}


it_status_t
itx_init_tracing( const char* aContextName,
		  int   aTraceRank )
{
  return IT_SUCCESS;
}
/********************************************************************/
