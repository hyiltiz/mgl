#ifdef documentation
=========================================================================

       program: mglStandaloneDigIO.c
            by: justin gardner
     copyright: (c) 2006 Justin Gardner, Jonas Larsson (GPL see mgl/COPYING)
          date: 06/30/09
       purpose: Based on the mglPrivateDigIO.c code. This installs a 
                background process that reads and writes a NI digital
                IO device (based on NI-DAQmx Base -- you will need to install
                NI-DAQmx Base to compile and use. I have been using this on
                an NI USB device (NI USB-6501 24-line Digital I/O). See the
                MGL wiki for info on how to install the NI driver and use
                this code. Note that this runs outside of Matlab (as opposed
                to on a thread like mglPrivateDigIO. Matlab communicates
                to this process through a socket. This is so that we can
                run matlab in 64 bit since NI refuses to provide a 64 bit
                library for NI cards
			   
=========================================================================
#endif

/////////////////////////
//   include section   //
/////////////////////////
#include <stdio.h>
#include <pthread.h>
#include "/Applications/National Instruments/NI-DAQmx Base/includes/NIDAQmxBase.h"
#include <sys/socket.h>
#include <sys/un.h>
#import <Foundation/Foundation.h>

//-----------------------------------------------------------------------------------///
// **************************** mac cocoa specific code  **************************** //
//-----------------------------------------------------------------------------------///
#ifdef __APPLE__

////////////////////////
//   define section   //
////////////////////////
#define INIT 1
#define DIGIN 2
#define DIGOUT 3
#define LIST 4
#define QUIT 0
#define SHUTDOWN -1
// NIDAQ error checking macro
#define DAQmxErrChk(functionCall) { if( DAQmxFailed(error=(functionCall)) ) { goto Error; } }
// event types
#define DIGDOWN_EVENT 0
#define DIGUP_EVENT 1
#define DIGOUT_EVENT 2
#define QUIT_EVENT 3
#define INIT_EVENT 4

/////////////////////
//   queue event   //
/////////////////////
@interface digQueueEvent : NSObject {
  int type;
  double time;
  uInt32 val;
}
- (id)initWithTypeTimeAndValue:(int)initType :(double)initTime :(uInt32)initVal;
- (id)initWithTypeAndValue:(int)initType :(uInt32)initVal;
- (id)initWithType:(int)initType;
- (double)time;
- (uInt32)val;
- (int)eventType;
- (void)doEvent;
- (NSComparisonResult)compareByTime:(digQueueEvent *)otherQueueEvent;
- (void)dealloc;
@end

///////////////////////////////
//   function declarations   //
///////////////////////////////
double getCurrentTimeInSeconds();
int nidaqStartTask();
void nidaqStopTask();
void endDigIO(void);
void initDigIO(void);
void logDigIO(void);
void digin(void); 
void digout(void);
void diglist(void);
void digquit(void);
int openSocket(char *socketName);
void processEvent(void);

////////////////
//   globals  //
////////////////
static pthread_mutex_t digioMutex;
static NSAutoreleasePool *gDigIOPool;
static NSMutableArray *gDiginEventQueue;
static NSMutableArray *gDigoutEventQueue;
// NIDAQ specific globals
static TaskHandle nidaqInputTaskHandle = 0,nidaqOutputTaskHandle = 0;
static int nidaqInputPortNum = 1,nidaqOutputPortNum = 2;
static int stopNidaqThread = 0;
static uInt8 nidaqInputStatePrevious[1] = {0};

//////////////
//   main   //
//////////////
int main(int argc, char *argv[])
{
  // declare local variables
  int socketDescriptor;

  // init digIO
  initDigIO();    

  // open the communication socket, checking for error
  socketDescriptor = openSocket(".mglDigIO");
  if (socketDescriptor == 0) return;
  
  double startTimeInSeconds = getCurrentTimeInSeconds();
  while ((getCurrentTimeInSeconds()-startTimeInSeconds) < 10.0) {
    // log any dig IO event there is
    logDigIO();
    // process events
    processEvent();
  }
    

  // close socket
  close(socketDescriptor);

  // end digIO
  endDigIO();    
  
  return(0);
}

////////////////////
//    logDigIO    //
////////////////////
void logDigIO(void)
{
  // read the port once to get current state
  int32       read;
  // if the nidaq input task handle has been initialized, then read the port
  if (nidaqInputTaskHandle != 0) {
    // read current state of digio port
    uInt8 nidaqInputState[1];
    DAQmxBaseReadDigitalU8(nidaqInputTaskHandle,1,0.01,DAQmx_Val_GroupByChannel,nidaqInputState,1,&read,NULL);
    // see if it is different from previous state
    if (nidaqInputState[0] != nidaqInputStatePrevious[0]) {
      // check which bit has changes
      int bitnum;
      for (bitnum = 0;bitnum < 8;bitnum++) {
	if (((nidaqInputStatePrevious[0]>>bitnum)&0x1) != ((nidaqInputState[0]>>bitnum)&0x1)) {
	  if ((nidaqInputState[0]>>bitnum)&0x1) {
	    // add a digup event
	    digQueueEvent *qEvent = [[digQueueEvent alloc] initWithTypeAndValue:DIGUP_EVENT :bitnum];
	    [gDiginEventQueue insertObject:qEvent atIndex:0];
	    [qEvent release];
	  }
	  else {
	    // add a digdown event
	    digQueueEvent *qEvent = [[digQueueEvent alloc] initWithTypeAndValue:DIGDOWN_EVENT :bitnum];
	    [gDiginEventQueue insertObject:qEvent atIndex:0];
	    [qEvent release];
	  }
	}
      }
      nidaqInputStatePrevious[0] = nidaqInputState[0];
    }
  }
}

//////////////////////
//   processEvent   //
//////////////////////
void processEvent(void)
{
  double currentTimeInSeconds;

  // get the current time in seconds
  currentTimeInSeconds = getCurrentTimeInSeconds();
  // check for events to process
  if ([gDigoutEventQueue count] > 0) {
    // see if we need to post the top element on the queue
    if (currentTimeInSeconds > [[gDigoutEventQueue objectAtIndex:0] time]) {
      // set the port
      [[gDigoutEventQueue objectAtIndex:0] doEvent];
      // and remove event from the queue
      [gDigoutEventQueue removeObjectAtIndex:0];
    }
  }
}
 
///////////////////////////////////
//   queue event implementation  //
///////////////////////////////////
@implementation digQueueEvent 
- (id)initWithType:(int)initType
{
  // init parent
  [super init];
  // set internals
  type = initType;
  time = getCurrentTimeInSeconds();
  val = 0;
  //return self
  return self;
}
- (id)initWithTypeTimeAndValue:(int)initType :(double)initTime :(uInt32)initVal;
{
  // init parent
  [super init];
  // set internals
  type = initType;
  time = initTime;
  val = initVal;
  //return self
  return self;
}
- (id)initWithTypeAndValue:(int)initType :(uInt32)initVal; 
{
  // init parent
  [super init];
  // set internals
  type = initType;
  time = getCurrentTimeInSeconds();
  val = initVal;
  //return self
  return self;
}
- (int)eventType
{
  return type;
}
- (double)time
{
  return time;
}
- (uInt32)val
{
  return val;
}
- (void)dealloc
{
  [super dealloc];
}
- (void)doEvent
{
  if (type == DIGOUT_EVENT) {
    int32       written;
    // DAQmxBaseWriteDigitalU8 
    DAQmxBaseWriteDigitalU32(nidaqOutputTaskHandle,1,1,10.0,DAQmx_Val_GroupByChannel,&val,&written,NULL);
    return;
  }
}
// comparison function, used to sort the queue in time order
- (NSComparisonResult)compareByTime:(digQueueEvent*)otherQueueEvent
{
  if ([self time] > [otherQueueEvent time])  {
    return NSOrderedDescending;
  }
  else if ([self time] == [otherQueueEvent time]) {
    return NSOrderedSame;
  }
  else {
    return NSOrderedAscending;
  }
}
@end

////////////////////////
//   getCurrentTime   //
////////////////////////
double getCurrentTimeInSeconds()
{
  // get current time
  UnsignedWide currentTime; 
  Microseconds(&currentTime); 

  // convert microseconds to double
  double twoPower32 = 4294967296.0; 
  double doubleValue; 
  
  double upperHalf = (double)currentTime.hi; 
  double lowerHalf = (double)currentTime.lo; 
  doubleValue = (upperHalf * twoPower32) + lowerHalf; 
  return(0.000001*doubleValue);
}


/////////////////////////
//   nidaqStartTask   //
/////////////////////////
int nidaqStartTask()
{
  // Error variables
  int32       error = 0;
  char        errBuff[2048];

  // write variables
  int32       written;
  uInt32 val;

  // Setup the channel parameter
  char inputChannel[256];
  sprintf(inputChannel,"Dev1/port%i",nidaqInputPortNum);
  char outputChannel[256];
  sprintf(outputChannel,"Dev1/port%i",nidaqOutputPortNum);

  if (nidaqInputTaskHandle != 0) {
    printf("(mglStandaloneDigIO) DigIO already open, shutting down and restarting\n");
    nidaqStopTask;
  }
		   
  // open as a digital input
  DAQmxErrChk (DAQmxBaseCreateTask ("", &nidaqInputTaskHandle));
  return 0;
  DAQmxErrChk (DAQmxBaseCreateDIChan(nidaqInputTaskHandle,inputChannel,"",DAQmx_Val_ChanForAllLines));
  DAQmxErrChk (DAQmxBaseStartTask (nidaqInputTaskHandle));

  // Create the output task
  DAQmxErrChk (DAQmxBaseCreateTask ("", &nidaqOutputTaskHandle));
  DAQmxErrChk (DAQmxBaseCreateDOChan(nidaqOutputTaskHandle,outputChannel,"",DAQmx_Val_ChanForAllLines));
  DAQmxErrChk (DAQmxBaseStartTask (nidaqOutputTaskHandle));
  
  // return success
  return 1;

 Error:

   if (DAQmxFailed (error))
     DAQmxBaseGetExtendedErrorInfo (errBuff, 2048);

   nidaqStopTask();

   // output error, but only if it is not device idnetifier is invalid
   // since this happens when you simply don't have a card in the
   // computer
   if (error)
     if (error != -200220)
       printf("(mglStandaloneDigIO) DAQmxBase Error %d: %s\n", (int)error, errBuff);
     else
       printf("(mglStandaloneDigIO) No device found. DAQmxBase Error %d: %s\n", (int)error, errBuff);
       
   return 0;
}

///////////////////////
//   nidaqStopTask   //
///////////////////////
void nidaqStopTask()
{
  if (nidaqInputTaskHandle != 0) {
    // stop input task
    DAQmxBaseStopTask (nidaqInputTaskHandle);
    DAQmxBaseClearTask(nidaqInputTaskHandle);
    nidaqInputTaskHandle = 0;
  }
  if (nidaqOutputTaskHandle != 0) {
    // stop output task
    DAQmxBaseStopTask (nidaqOutputTaskHandle);
    DAQmxBaseClearTask(nidaqOutputTaskHandle);
    nidaqOutputTaskHandle = 0;
  }
}

////////////////////
//    initDigIO   // 
////////////////////
void initDigIO(void)
{
  // start the thread that will continue to handle reading and
  // writing the NIDAQ card
  printf("(mglStandaloneDigIO) Starting DigIO\n");
  // turn off flag to shutdown thread
  stopNidaqThread = 0;
  // init pthread_mutex
  pthread_mutex_init(&digioMutex,NULL);
  // init the event queue
  gDigIOPool = [[NSAutoreleasePool alloc] init];
  gDiginEventQueue = [[NSMutableArray alloc] init];
  gDigoutEventQueue = [[NSMutableArray alloc] init];

  // FIX FIX FIX: add ability to set what ports to listen and write to
  // get the nidaq ports

  // display message
  printf("(mglStandaloneDigIO) Initializing digin port: Dev1/port%i digout port: Dev1/port%i. End with mglDigIO('quit').\n",nidaqInputPortNum,nidaqOutputPortNum);
  // and attempt to start task
  if (nidaqStartTask() == 0) {
    printf("============================================================================\n");
    printf("(mglStandaloneDigIO) UHOH! Could not start NIDAQ ports digin: %i and digout: %i\n",nidaqInputPortNum,nidaqOutputPortNum);
    printf("============================================================================\n");
  }
}

//////////////////
//   endDigIO   //
//////////////////
void endDigIO(void)
{
  // clear the queues
  [gDigoutEventQueue removeAllObjects];
  [gDiginEventQueue removeAllObjects];
  // release the event queue
  [gDigoutEventQueue release];
  [gDiginEventQueue release];
  [gDigIOPool drain];
  // stop nidaq task
  nidaqStopTask();
}

////////////////
//    digin   // 
////////////////
void digin(void) 
{
#if 0
  // lock the mutex to avoid concurrent access to the global variables
  pthread_mutex_lock(&digioMutex);
  // see how many events we have
  unsigned count = [gDiginEventQueue count];
  // if we have more than one,
  if (count > 0) {
    int i = 0;
    // return event as a matlab structure
    const char *fieldNames[] =  {"type","line","when"};
    int outDims[2] = {1, 1};
    plhs[0] = mxCreateStructArray(1,outDims,3,fieldNames);
    
    mxSetField(plhs[0],0,"type",mxCreateDoubleMatrix(1,count,mxREAL));
    double *typeOut = (double*)mxGetPr(mxGetField(plhs[0],0,"type"));
    mxSetField(plhs[0],0,"line",mxCreateDoubleMatrix(1,count,mxREAL));
    double *lineOut = (double*)mxGetPr(mxGetField(plhs[0],0,"line"));
    mxSetField(plhs[0],0,"when",mxCreateDoubleMatrix(1,count,mxREAL));
    double *whenOut = (double*)mxGetPr(mxGetField(plhs[0],0,"when"));
    while (count--) {
      digQueueEvent *qEvent;
      // get the last event
      qEvent = [gDiginEventQueue objectAtIndex:0];
      // and get the value and time
      typeOut[i] = [qEvent eventType];
      lineOut[i] = [qEvent val];
      whenOut[i++] = [qEvent time];
      // remove it from the queue
      [gDiginEventQueue removeObjectAtIndex:0];
    }
    // release the mutex
    pthread_mutex_unlock(&digioMutex);
  }
  else {
    // no event found, unlock mutex and return empty
    pthread_mutex_unlock(&digioMutex);
    plhs[0] = mxCreateDoubleMatrix(0,0,mxREAL);
  }
#endif
} 

/////////////////
//    digout   // 
/////////////////
void digout(void)
{
#if 0
  // get value and time
  double time = (double)mxGetScalar(prhs[1]);
  uInt32 val = (uInt32)(double)mxGetScalar(prhs[2]);

  // lock the mutex to avoid concurrent access to the global variables
  pthread_mutex_lock(&digioMutex);

  // create the event
  digQueueEvent *qEvent = [[digQueueEvent alloc] initWithTypeTimeAndValue:DIGOUT_EVENT :time :val];

  // add the event to the event queue
  [gDigoutEventQueue addObject:qEvent];
  [qEvent release];

  // sort the event queue by time
  SEL compareByTime = @selector(compareByTime:);
  [gDigoutEventQueue sortUsingSelector:compareByTime];

  // release mutex
  pthread_mutex_unlock(&digioMutex);
  // return 1
  plhs[0] = mxCreateDoubleMatrix(1,1,mxREAL);
  *mxGetPr(plhs[0]) = 1;
#endif
}

//////////////////
//    diglist   // 
//////////////////
void diglist(void)
{
#if 0
  // lock the mutex to avoid concurrent access to the global variables
  pthread_mutex_lock(&digioMutex);
  // display which ports we are using
  printf("(mglStandaloneDigIO) DigIO thread is running\n");
  if (nidaqInputTaskHandle != 0) {
    // see if nidaq card is running
    printf("(mglPrivtateDigIO) Input port is: Dev1/port%i. Output port is: Dev1/port%i\n",nidaqInputPortNum,nidaqOutputPortNum);
    if ([gDigoutEventQueue count] == 0) {
      printf("(mglStandaloneDigIO) No digiout events pending.\n");
    }
    else {
      int i;
      for(i = 0; i < [gDigoutEventQueue count]; i++) {
	printf("(mglStandaloneDigIO) Set output port to %i is pending in %f seconds.\n",[[gDigoutEventQueue objectAtIndex:i] val],[[gDigoutEventQueue objectAtIndex:i] time] - getCurrentTimeInSeconds());
      }
    }
    // check input events
    printf("(mglStandaloneDigIO) %i digin events in queue\n",[gDiginEventQueue count]);
  }
  else
    printf("(mglStandaloneDigIO) NIDAQ card is not initialized.\n");
  // release mutex
  pthread_mutex_unlock(&digioMutex);
  // return 1
  plhs[0] = mxCreateDoubleMatrix(1,1,mxREAL);
  *mxGetPr(plhs[0]) = 1;
#endif
}

//////////////////
//    digquit   // 
//////////////////
void digquit(void)
{
#if 0
  // lock the mutex to avoid concurrent access to the global variables
  pthread_mutex_lock(&digioMutex);
    
  // add a quit event
  digQueueEvent *qEvent = [[digQueueEvent alloc] initWithType:QUIT_EVENT];
  [gDigoutEventQueue insertObject:qEvent atIndex:0];
  [qEvent release];

  // release mutex
  pthread_mutex_unlock(&digioMutex);
#endif
}

//////////////////////
//    openSocket    //
//////////////////////
int openSocket(char *socketName)
{
  struct sockaddr_un socketAddress;
  int socketDescriptor;

  // create socket and check for error
  if ((socketDescriptor = socket(AF_UNIX, SOCK_STREAM, 0)) == -1) {
    perror("(mglStandaloneDigIO) Could not create socket to communicate between matlab and mglStandaloneDigIO");
    return 0;
  }

  // set up socket address
  memset(&socketAddress, 0, sizeof(socketAddress));
  socketAddress.sun_family = AF_UNIX;
  strncpy(socketAddress.sun_path, socketName, sizeof(socketAddress.sun_path)-1);

  // unlink (make sure that it doesn't already exist)
  unlink(socketName);

  // bind the socket to the address, this could fail if you don't have
  // write permission to the directory where the socket is being made
  if (bind(socketDescriptor, (struct sockaddr*)&socketAddress, sizeof(socketAddress)) == -1) {
    printf("(mglStandaloneDigIO) Could not bind socket to name %s. This prevents communication between matlab and mglStandaloneDigIO. This might have happened because you do not have permission to write the file %s",socketName,socketName);
    perror(NULL);
    close(socketDescriptor);
    return 0;
  }

  // listen to the socket (accept up to 500 connects)
  if (listen(socketDescriptor, 500) == -1) {
    printf("(mglStandaloneDigIO) Could not listen to socket %s, which is used to communicate between matlab and mglStandaloneDigIO.",socketName);
    perror(NULL);
    close(socketDescriptor);
    return 0;
  }

  printf("(mglStandaloneDigIO) Opened socket %s\n",socketName);

  return socketDescriptor;
}

#else// __APPLE__
//-----------------------------------------------------------------------------------///
// ***************************** other-os specific code  **************************** //
//-----------------------------------------------------------------------------------///
// THIS FUNCTION IS ONLY FOR MAC COCOA
//////////////
//   main   //
//////////////
void mexFunction(int nlhs, mxArray *plhs[], int nrhs, const mxArray *prhs[])
{
  plhs[0] = mxCreateDoubleMatrix(1,1,mxREAL);
  *(double*)mxGetPr(plhs[0]) = 0;
}
#endif// __APPLE__


