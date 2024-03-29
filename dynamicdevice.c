#include "dynamicdevice.h"
#include "monitor.h"
#include "status.h"
#include <glob.h>
#include <vdr/skins.h>
#include <vdr/transfer.h>

#define SUBDEVICEREADYTIMEOUT    30 // seconds to wait until subdevice is ready

cPlugin *cDynamicDevice::dynamite = NULL;
int cDynamicDevice::defaultGetTSTimeout = 0;
int cDynamicDevice::idleTimeoutMinutes = 0;
int cDynamicDevice::idleWakeupHours = 0;
cString *cDynamicDevice::idleHook = NULL;
cString *cDynamicDevice::attachHook = NULL;
cDvbDeviceProbe *cDynamicDevice::dvbprobe = NULL;
bool cDynamicDevice::enableOsdMessages = false;
int cDynamicDevice::numDynamicDevices = 0;
cMutex cDynamicDevice::arrayMutex;
cDynamicDevice *cDynamicDevice::dynamicdevice[MAXDEVICES] = { NULL };
cList<cDynamicDeviceProbe::cDynamicDeviceProbeItem> cDynamicDevice::commandRequeue;

cList<cDynamicDevice::cDelayedDeviceItems> cDynamicDevice::cDelayedDeviceItems::delayedItems;

cDynamicDevice::cDelayedDeviceItems::cDelayedDeviceItems(const char *DevPath, int AttachDelay)
 :devPath(DevPath)
{
 dontAttachBefore = time(NULL) + AttachDelay;
 delayedItems.Add(this);
}

int cDynamicDevice::cDelayedDeviceItems::CanBeAttached(const char *DevPath)
{
  if (DevPath == NULL)
     return false;
  time_t now = time(NULL);
  for (cDelayedDeviceItems *item = delayedItems.First(); item; item = delayedItems.Next(item)) {
      if (strcmp(*item->devPath, DevPath) == 0) {
         if (item->dontAttachBefore < now) {
            delayedItems.Del(item);
            isyslog("dynamite: %s can be attached now", DevPath);
            return 1;
            }
         dsyslog("dynamite: %s should not be attached yet", DevPath);
         return 0;
         }
      }
  return 2;
}

int cDynamicDevice::IndexOf(const char *DevPath, int &NextFreeIndex, int WishIndex)
{
  cMutexLock lock(&arrayMutex);
  NextFreeIndex = -1;
  int index = -1;
  for (int i = 0; (i < numDynamicDevices) && ((index < 0) || (NextFreeIndex < 0) || (WishIndex >= 0)); i++) {
      if (dynamicdevice[i]->devpath == NULL) {
         if ((NextFreeIndex < 0) || ((WishIndex >= 0) && (dynamicdevice[i]->CardIndex() == WishIndex))) {
            NextFreeIndex = i;
            if ((dynamicdevice[i]->CardIndex() == WishIndex) && (index >= 0))
               break;
            }
         }
      else if (index < 0) {
         if (strcmp(DevPath, **dynamicdevice[i]->devpath) == 0)
            index = i;
         }
      }
  return index;
}

cDynamicDevice *cDynamicDevice::GetDynamicDevice(int Index)
{
  if ((Index < 0) || (Index >= numDynamicDevices))
     return NULL;
  return dynamicdevice[Index];
}

bool cDynamicDevice::ProcessQueuedCommands(void)
{
  for (cDynamicDeviceProbe::cDynamicDeviceProbeItem *dev = cDynamicDeviceProbe::commandQueue.First(); dev; dev = cDynamicDeviceProbe::commandQueue.Next(dev)) {
      switch (dev->cmd) {
         case ddpcAttach:
          {
           int delayed = cDelayedDeviceItems::CanBeAttached(*dev->devpath);
           if (delayed == 0)
              commandRequeue.Add(new cDynamicDeviceProbe::cDynamicDeviceProbeItem(ddpcAttach, new cString(*dev->devpath)));
           else if (delayed > 0)
              AttachDevice(*dev->devpath, delayed);
           break;
          }
         case ddpcDetach:
          {
           DetachDevice(*dev->devpath, false);
           break;
          }
         case ddpcService:
          {
           if (dynamite && (dev->devpath != NULL) && (**dev->devpath != NULL)) {
              int len = strlen(*dev->devpath);
              if (len > 0) {
                 char *data = strchr(const_cast<char*>(**dev->devpath), ' ');
                 if (data != NULL) {
                    data[0] = '\0';
                    data++;
                    dynamite->Service(*dev->devpath, data);
                    }
                 }
              }
           break;
          }
        }
      }
  cDynamicDeviceProbe::commandQueue.Clear();
  for (cDynamicDeviceProbe::cDynamicDeviceProbeItem *dev = commandRequeue.First(); dev; dev = commandRequeue.Next(dev))
      cDynamicDeviceProbe::commandQueue.Add(new cDynamicDeviceProbe::cDynamicDeviceProbeItem(dev->cmd, new cString(**dev->devpath)));
  commandRequeue.Clear();
  return true;
}

int cDynamicDevice::GetUdevAttributesForAttach(const char *DevPath, int &CardIndex, int &AttachDelay, bool &AttachDelayPreopen)
{
  CardIndex = -1;
  AttachDelay = 0;
  AttachDelayPreopen = false;
  if (DevPath == NULL)
     return -1;
  cUdevDevice *dev = cUdev::GetDeviceFromDevName(DevPath);
  if (dev == NULL)
     return -1;
  int intVal;
  const char *val = dev->GetPropertyValue("dynamite_cardindex");
  if (val) {
     isyslog("dynamite: udev cardindex is %s", val);
     intVal = -1;
     if (val && (sscanf(val, "%d", &intVal) == 1) && (intVal >= 0) && (intVal <= MAXDEVICES))
        CardIndex = intVal;
     }
  val = dev->GetPropertyValue("dynamite_attach_delay");
  if (val) {
     isyslog("dynamite: udev attach_delay is %s", val);
     intVal = 0;
     if (val && (sscanf(val, "%d", &intVal) == 1) && (intVal > 0))
        AttachDelay = intVal;
     }
  val = dev->GetPropertyValue("dynamite_attach_delay_preopen");
  if (val) {
     isyslog("dynamite: udev attach_delay_preopen is %s", val);
     if ((strcmp(val, "1") == 0) || (strcasecmp(val, "y") == 0) || (strcasecmp(val, "yes") == 0))
        AttachDelayPreopen = true;
     }
  delete dev;
  return 0;
}

void cDynamicDevice::DetachAllDevices(bool Force)
{
  cMutexLock lock(&arrayMutex);
  isyslog("dynamite: %sdetaching all devices", (Force ? "force " : ""));
  for (int i = 0; i < numDynamicDevices; i++) {
      if (dynamicdevice[i]->devpath) {
         if (Force)
            cDynamicDeviceProbe::QueueDynamicDeviceCommand(ddpcService, *cString::sprintf("dynamite-ForceDetachDevice-v0.1 %s", **dynamicdevice[i]->devpath));
         else
            cDynamicDeviceProbe::QueueDynamicDeviceCommand(ddpcDetach, (**dynamicdevice[i]->devpath));
         }
      }
}

cString cDynamicDevice::ListAllDevices(int &ReplyCode)
{
  cMutexLock lock(&arrayMutex);
  cString devices;
  int count = 0;
  for (int i = 0; i < numDynamicDevices; i++) {
      if ((dynamicdevice[i]->devpath != NULL) && (dynamicdevice[i]->subDevice != NULL)) {
         count++;
         devices = cString::sprintf("%s%d%s %s\n", (count == 1) ? "" : *devices
                                                 , i + 1
                                                 , ((PrimaryDevice() == dynamicdevice[i]) || !dynamicdevice[i]->isDetachable) ? "*" : ""
                                                 , **dynamicdevice[i]->devpath);
         }
      }
  if (count == 0) {
     ReplyCode = 901;
     return cString::sprintf("there are no attached devices");
     }
  return devices;
}

cString cDynamicDevice::AttachDevicePattern(const char *Pattern)
{
  if (!Pattern)
     return "invalid pattern";
  cStringList paths;
  cString reply;
  glob_t result;
  if (glob(Pattern, GLOB_MARK, 0, &result) == 0) {
     for (uint g = 0; g < result.gl_pathc; g++)
         paths.Append(strdup(result.gl_pathv[g]));
     paths.Sort(false);
     for (int i = 0; i < paths.Size(); i++) {
         cDynamicDeviceProbe::QueueDynamicDeviceCommand(ddpcAttach, paths[i]);
         reply = cString::sprintf("%squeued %s for attaching\n", (i == 0) ? "" : *reply, paths[i]);
         }
     }
  globfree(&result);
  return reply;
}

eDynamicDeviceReturnCode cDynamicDevice::AttachDevice(const char *DevPath, int Delayed)
{
  if (!DevPath)
     return ddrcNotSupported;

  cMutexLock lock(&arrayMutex);
  int freeIndex = -1;
  int index = -1;
  bool isDvbDevice = false;
  int adapter = -1;
  int frontend = -1;
  int wishIndex = -1;
  int attachDelay = 0;
  bool attachDelayPreopen = false;
  GetUdevAttributesForAttach(DevPath, wishIndex, attachDelay, attachDelayPreopen);
  if (wishIndex >= 0)
     isyslog("dynamite: %s wants card index %d", DevPath, wishIndex);
  else if (sscanf(DevPath, "/dev/dvb/adapter%d/frontend%d", &adapter, &frontend) == 2) {
     isDvbDevice = false;
     wishIndex = adapter;
     isyslog("dynamite: %s is a dvb adapter trying to set card index to %d", DevPath, wishIndex);
     }
  index = IndexOf(DevPath, freeIndex, wishIndex);

  if (index >= 0) {
     isyslog("dynamite: %s is already attached", DevPath);
     return ddrcAlreadyAttached;
     }

  if (freeIndex < 0) {
     esyslog("dynamite: no more free slots for %s", DevPath);
     return ddrcNoFreeDynDev;
     }

  if ((attachDelay > 0) && (Delayed > 1)) {
     if (attachDelayPreopen) {
        // trigger firmware load
        isyslog("dynamite: open %s before attach", DevPath);
        int fd = open(DevPath, O_RDWR | O_NONBLOCK);
        if (fd > 0) {
           close(fd);
           isyslog("dynamite: close %s", DevPath);
           }
        }
     commandRequeue.Add(new cDynamicDeviceProbe::cDynamicDeviceProbeItem(ddpcAttach, new cString(DevPath)));
     new cDelayedDeviceItems(DevPath, attachDelay);
     return ddrcAttachDelayed;
     }

  cUdevDevice *dev = cUdev::GetDeviceFromDevName(DevPath);
  if (dev != NULL) {
     bool ignore = false;
     const char *tmp;
     if (((tmp = dev->GetPropertyValue("dynamite_attach")) != NULL)
      && ((strcmp(tmp, "0") == 0) || (strcasecmp(tmp, "n") == 0)
       || (strcasecmp(tmp, "no") == 0) || (strcasecmp(tmp, "ignore") == 0))) {
        isyslog("dynamite: udev says don't attach %s", DevPath);
        ignore = true;
        }
     else if (((tmp = dev->GetPropertyValue("dynamite_instanceid")) != NULL) && isnumber(tmp)) {
        int devInstanceId = strtol(tmp, NULL, 10);
        if (devInstanceId != InstanceId) {
           isyslog("dynamite: device %s is for vdr instance %d, we are %d", DevPath, devInstanceId, InstanceId);
           ignore = true;
           }
        }
     delete dev;
     if (ignore)
        return ddrcNotSupported;
     }

  cDevice::nextParentDevice = dynamicdevice[freeIndex];
  
  for (cDynamicDeviceProbe *ddp = DynamicDeviceProbes.First(); ddp; ddp = DynamicDeviceProbes.Next(ddp)) {
      if (ddp->Attach(dynamicdevice[freeIndex], DevPath))
         goto attach; // a plugin has created the actual device
      }

  // if it's a dvbdevice try the DvbDeviceProbes as a fallback for unpatched plugins
  if (isDvbDevice || (sscanf(DevPath, "/dev/dvb/adapter%d/frontend%d", &adapter, &frontend) == 2)) {
     for (cDvbDeviceProbe *dp = DvbDeviceProbes.First(); dp; dp = DvbDeviceProbes.Next(dp)) {
         if (dp != dvbprobe) {
            if (dp->Probe(adapter, frontend))
               goto attach;
            }
         }
     new cDvbDevice(adapter, frontend, dynamicdevice[freeIndex]);
     goto attach;
     }

  esyslog("dynamite: can't attach %s", DevPath);
  return ddrcNotSupported;

attach:
  int retry = 3;
  do {
     dynamicdevice[freeIndex]->lastCloseDvr = time(NULL);
     for (time_t t0 = time(NULL); time(NULL) - t0 < SUBDEVICEREADYTIMEOUT; ) {
         if (dynamicdevice[freeIndex]->Ready()) {
            retry = -1;
            break;
            }
         cCondWait::SleepMs(100);
         }
     if (!dynamicdevice[freeIndex]->Ready() && dynamicdevice[freeIndex]->HasCi() && (retry > 0)) {
        retry--;
        isyslog("dynamite: device %s not ready after %d seconds - resetting CAMs (retry == %d)", DevPath, SUBDEVICEREADYTIMEOUT, retry);
        for (cCamSlot* cs = CamSlots.First(); cs; cs = CamSlots.Next(cs)) {
            if ((cs->Device() == dynamicdevice[freeIndex]) || (cs->Device() == NULL))
               cs->Reset();
            }
        }
     else
        break;
     } while (retry >= 0);
  dynamicdevice[freeIndex]->devpath = new cString(DevPath);
  isyslog("dynamite: attached device %s to dynamic device slot %d", DevPath, freeIndex + 1);
  dynamicdevice[freeIndex]->ReadUdevProperties();
  cPluginManager::CallAllServices("dynamite-event-DeviceAttached-v0.1", (void*)DevPath);
  cDvbDevice::BondDevices(Setup.DeviceBondings); // "re-bond"
  if (enableOsdMessages) {
     cString osdMsg = cString::sprintf(tr("attached %s"), DevPath);
     Skins.QueueMessage(mtInfo, *osdMsg);
     }
  cDynamiteStatus::SetStartupChannel();
  if (attachHook != NULL) {
     cString hookCmd = cString::sprintf("%s --action=attach --device=%s", **attachHook, DevPath);
     isyslog("dynamite: calling hook %s", *hookCmd);
     int status = SystemExec(*hookCmd, true);
     if (!WIFEXITED(status) || WEXITSTATUS(status))
        esyslog("SystemExec() failed with status %d", status);
     }
  dynamicdevice[freeIndex]->subDeviceIsReady = true;
  return ddrcSuccess;
}

eDynamicDeviceReturnCode cDynamicDevice::DetachDevice(const char *DevPath, bool Force)
{
  if (!DevPath)
     return ddrcNotSupported;

  cMutexLock lock(&arrayMutex);
  int freeIndex = -1;
  int index = -1;
  if (isnumber(DevPath))
     index = strtol(DevPath, NULL, 10) - 1;
  else
     index = IndexOf(DevPath, freeIndex, -1);

  if ((index < 0) || (index >= numDynamicDevices)) {
     esyslog("dynamite: device %s not found", DevPath);
     return ddrcNotFound;
     }

  cString realDevPath(dynamicdevice[index]->GetDevPath());
  if (!Force) {
     if (!dynamicdevice[index]->isDetachable) {
        esyslog("dynamite: detaching of device %s is not allowed", *realDevPath);
        return ddrcNotAllowed;
        }

     if (dynamicdevice[index] == PrimaryDevice()) {
        esyslog("dynamite: detaching of primary device %s is not supported", *realDevPath);
        return ddrcIsPrimaryDevice;
        }

     if (dynamicdevice[index]->Receiving(false)) {
        esyslog("dynamite: can't detach device %s, it's receiving something important", *realDevPath);
        return ddrcIsReceiving;
        }
     }

  dynamicdevice[index]->DeleteSubDevice();
  isyslog("dynamite: detached device %s%s", *realDevPath, (Force ? " (forced)" : ""));
  if (enableOsdMessages) {
     cString osdMsg = cString::sprintf(tr("detached %s"), *realDevPath);
     Skins.QueueMessage(mtInfo, *osdMsg);
     }
  if (attachHook != NULL) {
     cString hookCmd = cString::sprintf("%s --action=detach --device=%s", **attachHook, *realDevPath);
     isyslog("dynamite: calling hook %s", *hookCmd);
     int status = SystemExec(*hookCmd, true);
     if (!WIFEXITED(status) || WEXITSTATUS(status))
        esyslog("SystemExec() failed with status %d", status);
     }
  return ddrcSuccess;
}

eDynamicDeviceReturnCode cDynamicDevice::SetLockDevice(const char *DevPath, bool Lock)
{
  if (!DevPath)
     return ddrcNotSupported;

  cMutexLock lock(&arrayMutex);
  int freeIndex = -1;
  int index = -1;
  if (isnumber(DevPath))
     index = strtol(DevPath, NULL, 10) - 1;
  else
     index = IndexOf(DevPath, freeIndex, -1);

  if ((index < 0) || (index >= numDynamicDevices))
     return ddrcNotFound;

  dynamicdevice[index]->InternSetLock(Lock);
  return ddrcSuccess;
}

static void CallIdleHook(const char *IdleHook, const char *DevPath, bool Idle)
{
  cString idleHookCmd = cString::sprintf("%s --idle=%s --device=%s", IdleHook, (Idle ? "on" : "off"), DevPath);
  isyslog("dynamite: calling idle hook %s", *idleHookCmd);
  int status = SystemExec(*idleHookCmd, false);
  if (!WIFEXITED(status) || WEXITSTATUS(status))
     esyslog("SystemExec() failed with status %d", status);
}

eDynamicDeviceReturnCode cDynamicDevice::SetIdle(const char *DevPath, bool Idle)
{
  if (!DevPath)
     return ddrcNotSupported;

  cMutexLock lock(&arrayMutex);
  int freeIndex = -1;
  int index = -1;
  if (isnumber(DevPath))
     index = strtol(DevPath, NULL, 10) - 1;
  else
     index = IndexOf(DevPath, freeIndex, -1);

  if ((index < 0) || (index >= numDynamicDevices))
     return ddrcNotFound;

  isyslog("dynamite: set device %s to %s", DevPath, (Idle ? "idle" : "not idle"));
  if (idleHook && !Idle)
     CallIdleHook(**idleHook, dynamicdevice[index]->GetDevPath(), Idle);
  if (((cDevice*)dynamicdevice[index])->SetIdle(Idle)) {
     if (idleHook && Idle)
        CallIdleHook(**idleHook, dynamicdevice[index]->GetDevPath(), Idle);
     }
  else if (idleHook && !Idle)
     CallIdleHook(**idleHook, dynamicdevice[index]->GetDevPath(), Idle);
  if (Idle) {
     dynamicdevice[index]->idleSince = time(NULL);
     dynamicdevice[index]->lastCloseDvr = dynamicdevice[index]->idleSince;
     }
  else {
     dynamicdevice[index]->idleSince = 0;
     dynamicdevice[index]->lastCloseDvr = time(NULL);
     }
  return ddrcSuccess;
}

eDynamicDeviceReturnCode cDynamicDevice::SetAutoIdle(const char *DevPath, bool Disable)
{
  if (!DevPath)
     return ddrcNotSupported;

  cMutexLock lock(&arrayMutex);
  int freeIndex = -1;
  int index = -1;
  if (isnumber(DevPath))
     index = strtol(DevPath, NULL, 10) - 1;
  else
     index = IndexOf(DevPath, freeIndex, -1);

  if ((index < 0) || (index >= numDynamicDevices))
     return ddrcNotFound;

  isyslog("dynamite: %s auto-idle mode on device %s", (Disable ? "disable" : "enable"), DevPath);
  dynamicdevice[index]->disableAutoIdle = Disable;
  return ddrcSuccess;
}

void cDynamicDevice::AutoIdle(void)
{
  if (idleTimeoutMinutes <= 0)
     return;
  cMutexLock lock(&arrayMutex);
  time_t now = time(NULL);
  bool wokeupSomeDevice = false;
  int seconds = 0;
  for (int i = 0; i < numDynamicDevices; i++) {
      if ((dynamicdevice[i]->devpath != NULL) && !dynamicdevice[i]->disableAutoIdle) {
         if (dynamicdevice[i]->IsIdle()) {
            seconds = now - dynamicdevice[i]->idleSince;
            if ((dynamicdevice[i]->idleSince > 0) && (seconds >= (idleWakeupHours * 3600))) {
               isyslog("dynamite: device %s idle for %d hours, waking up", dynamicdevice[i]->GetDevPath(), seconds / 3600);
               cDynamicDeviceProbe::QueueDynamicDeviceCommand(ddpcService, *cString::sprintf("dynamite-SetNotIdle-v0.1 %s", dynamicdevice[i]->GetDevPath()));
               wokeupSomeDevice = true;
               }
            }
         else {
            seconds = now - dynamicdevice[i]->lastCloseDvr;
            if ((!dynamicdevice[i]->Occupied() ) && (dynamicdevice[i]->lastCloseDvr > 0) && (seconds >= (idleTimeoutMinutes * 60))) {
               if (dynamicdevice[i]->lastCloseDvr > 0)
                  isyslog("dynamite: device %s unused for %d minutes, set to idle", dynamicdevice[i]->GetDevPath(), seconds / 60);
               else
                  isyslog("dynamite: device %s never used , set to idle", dynamicdevice[i]->GetDevPath());
               cDynamicDeviceProbe::QueueDynamicDeviceCommand(ddpcService, *cString::sprintf("dynamite-SetIdle-v0.1 %s", dynamicdevice[i]->GetDevPath()));
               }
            }
         }
      }

  if (wokeupSomeDevice) {
     // initiate epg-scan?
     }
}

eDynamicDeviceReturnCode cDynamicDevice::SetGetTSTimeout(const char *DevPath, int Seconds)
{
  if (!DevPath || (Seconds < 0))
     return ddrcNotSupported;

  cMutexLock lock(&arrayMutex);
  int freeIndex = -1;
  int index = -1;
  if (isnumber(DevPath))
     index = strtol(DevPath, NULL, 10) - 1;
  else
     index = IndexOf(DevPath, freeIndex, -1);

  if ((index < 0) || (index >= numDynamicDevices))
     return ddrcNotFound;

  dynamicdevice[index]->InternSetGetTSTimeout(Seconds);
  return ddrcSuccess;
}

void cDynamicDevice::SetDefaultGetTSTimeout(int Seconds)
{
  if (Seconds >= 0) {
     defaultGetTSTimeout = Seconds;
     isyslog("dynamite: set default GetTS-Timeout to %d seconds", Seconds);
     cMutexLock lock(&arrayMutex);
     for (int i = 0; i < numDynamicDevices; i++)
         dynamicdevice[i]->InternSetGetTSTimeout(Seconds);
     }
}

eDynamicDeviceReturnCode cDynamicDevice::SetGetTSTimeoutHandlerArg(const char *DevPath, const char *Arg)
{
  if (!DevPath || !Arg)
     return ddrcNotSupported;

  cMutexLock lock(&arrayMutex);
  int freeIndex = -1;
  int index = -1;
  if (isnumber(DevPath))
     index = strtol(DevPath, NULL, 10) - 1;
  else
     index = IndexOf(DevPath, freeIndex, -1);

  if ((index < 0) || (index >= numDynamicDevices))
     return ddrcNotFound;

  dynamicdevice[index]->InternSetGetTSTimeoutHandlerArg(Arg);
  return ddrcSuccess;
}

bool cDynamicDevice::IsAttached(const char *DevPath)
{
  cMutexLock lock(&arrayMutex);
  int freeIndex = -1;
  int index = IndexOf(DevPath, freeIndex, -1);
  return ((index >= 0) && (index >= numDynamicDevices));
}

cDynamicDevice::cDynamicDevice()
:index(-1)
,subDeviceIsReady(false)
,devpath(NULL)
,udevRemoveSyspath(NULL)
,udevProvidesSources(NULL)
,getTSTimeoutHandlerArg(NULL)
,isDetachable(true)
,getTSTimeout(defaultGetTSTimeout)
,disableAutoIdle(false)
{
  index = numDynamicDevices;
  if (numDynamicDevices < MAXDEVICES) {
     dynamicdevice[index] = this;
     numDynamicDevices++;
     }
  else
     esyslog("dynamite: ERROR: too many dynamic devices!");
}

cDynamicDevice::~cDynamicDevice()
{
  DeleteSubDevice();
  if (getTSTimeoutHandlerArg)
     delete getTSTimeoutHandlerArg;
  getTSTimeoutHandlerArg = NULL;
}

const char *cDynamicDevice::GetDevPath(void) const
{
  return (devpath ? **devpath : "");
}

void cDynamicDevice::ReadUdevProperties(void)
{
  if (devpath == NULL)
     return;
  cUdevDevice *dev = cUdev::GetDeviceFromDevName(**devpath);
  if (dev != NULL) {
     const char *timeout = dev->GetPropertyValue("dynamite_timeout");
     int seconds = -1;
     if (timeout && (sscanf(timeout, "%d", &seconds) == 1) && (seconds >= 0))
        InternSetGetTSTimeout(seconds);

     const char *timeoutHandlerArg = dev->GetPropertyValue("dynamite_timeout_handler_arg");
     if (timeoutHandlerArg)
        InternSetGetTSTimeoutHandlerArg(timeoutHandlerArg);

     const char *disableAutoIdleArg = dev->GetPropertyValue("dynamite_disable_autoidle");
     if (disableAutoIdleArg && ((strcmp(disableAutoIdleArg, "y") == 0)
                             || (strcmp(disableAutoIdleArg, "yes") == 0)
                             || (strcmp(disableAutoIdleArg, "true") == 0)
                             || (strcmp(disableAutoIdleArg, "disable") == 0)
                             || (strcmp(disableAutoIdleArg, "1") == 0)))
        disableAutoIdle = true;

     const char *providesSources = dev->GetPropertyValue("dynamite_sources");
     if (providesSources) {
        if (udevProvidesSources)
           delete udevProvidesSources;
        udevProvidesSources = new cString(cString::sprintf(",%s,", providesSources));
        }

     cUdevDevice *p = dev->GetParent();
     if (p) {
        const char *subsystem = p->GetSubsystem();
        const char *syspath = p->GetSyspath();
        if (subsystem && syspath && (strcmp(subsystem, "usb") == 0)) {
           cUdevUsbRemoveFilter::AddItem(syspath, **devpath);
           if (udevRemoveSyspath)
              delete udevRemoveSyspath;
           udevRemoveSyspath = new cString(syspath);
           }
        }

     delete dev;
     }
}

void cDynamicDevice::InternSetGetTSTimeout(int Seconds)
{
  getTSTimeout = Seconds;
  if (subDevice == NULL)
     return; // no log message if no device is connected
  if (Seconds == 0)
     isyslog("dynamite: disable GetTSTimeout on device %s", GetDevPath());
  else
     isyslog("dynamite: set GetTSTimeout on device %s to %d seconds", GetDevPath(), Seconds);
}

void cDynamicDevice::InternSetGetTSTimeoutHandlerArg(const char *Arg)
{
  if (getTSTimeoutHandlerArg)
     delete getTSTimeoutHandlerArg;
  getTSTimeoutHandlerArg = new cString(Arg);
  isyslog("dynamite: set GetTSTimeoutHandlerArg on device %s to %s", GetDevPath(), Arg);
}

void cDynamicDevice::InternSetLock(bool Lock)
{
  isDetachable = !Lock;
  isyslog("dynamite: %slocked device %s", Lock ? "" : "un", GetDevPath());
}

bool cDynamicDevice::InternProvidesSource(int Source) const
{
  if (udevProvidesSources) {
     cString source = cSource::ToString(Source);
     cString search = cString::sprintf(",%s,", *source);
     if (strstr(**udevProvidesSources, *search) == NULL) {
        isyslog("dynamite: device %s shall not provide source %s", GetDevPath(), *source);
        return false;
        }
     }
  return true;
}

void cDynamicDevice::DeleteSubDevice()
{
  subDeviceIsReady = false;
  if (subDevice) {
     Cancel(3);
     if (cTransferControl::ReceiverDevice() == this)
        cControl::Shutdown();
     subDevice->StopSectionHandler();
     delete subDevice;
     subDevice = NULL;
     isyslog("dynamite: deleted device for %s", (devpath ? **devpath : "(unknown)"));
     if (devpath)
        cPluginManager::CallAllServices("dynamite-event-DeviceDetached-v0.1", (void*)**devpath);
     }
  if (udevRemoveSyspath) {
     cUdevUsbRemoveFilter::RemoveItem(**udevRemoveSyspath, GetDevPath());
     delete udevRemoveSyspath;
     udevRemoveSyspath = NULL;
     }
  if (udevProvidesSources) {
     delete udevProvidesSources;
     udevProvidesSources = NULL;
     }
  if (devpath) {
     delete devpath;
     devpath = NULL;
     }
  isDetachable = true;
  getTSTimeout = defaultGetTSTimeout;
  disableAutoIdle = false;
}

bool cDynamicDevice::SetIdleDevice(bool Idle, bool TestOnly)
{
  if (subDevice)
     return subDevice->SetIdleDevice(Idle, TestOnly);
  return false;
}

bool cDynamicDevice::ProvidesEIT(void) const
{
  if (subDevice)
     return subDevice->ProvidesEIT();
  return false;
}

void cDynamicDevice::MakePrimaryDevice(bool On)
{
  if (subDevice)
     subDevice->MakePrimaryDevice(On);
  cDevice::MakePrimaryDevice(On);
}

#if VDRVERSNUM > 20400
bool cDynamicDevice::IsBonded(void) const
{
  if (subDevice)
     return subDevice->IsBonded();
  return false;
}
#endif

bool cDynamicDevice::HasDecoder(void) const
{
  if (subDevice)
     return subDevice->HasDecoder();
  return cDevice::HasDecoder();
}

cString cDynamicDevice::DeviceType(void) const
{
  if (subDevice)
     return subDevice->DeviceType();
  return "dyn";
}

cString cDynamicDevice::DeviceName(void) const
{
  if (subDevice)
     return subDevice->DeviceName();
  return cString::sprintf("dynamite-cDynamicDevice-%d", index);
}

bool cDynamicDevice::AvoidRecording(void) const
{
  if (subDevice)
     return subDevice->AvoidRecording();
  return cDevice::AvoidRecording();
}

cSpuDecoder *cDynamicDevice::GetSpuDecoder(void)
{
  if (subDevice)
     return subDevice->GetSpuDecoder();
  return cDevice::GetSpuDecoder();
}

bool cDynamicDevice::HasCi(void)
{
  if (subDevice)
     return subDevice->HasCi();
  return cDevice::HasCi();
}

bool cDynamicDevice::HasInternalCam(void)
{
  if (subDevice)
     return subDevice->HasInternalCam();
  return cDevice::HasInternalCam();
}

uchar *cDynamicDevice::GrabImage(int &Size, bool Jpeg, int Quality, int SizeX, int SizeY)
{
  if (subDevice)
     return subDevice->GrabImage(Size, Jpeg, Quality, SizeX, SizeY);
  return cDevice::GrabImage(Size, Jpeg, Quality, SizeX, SizeY);
}

void cDynamicDevice::SetVideoDisplayFormat(eVideoDisplayFormat VideoDisplayFormat)
{
  if (subDevice)
     return subDevice->SetVideoDisplayFormat(VideoDisplayFormat);
  cDevice::SetVideoDisplayFormat(VideoDisplayFormat);
}

void cDynamicDevice::SetVideoFormat(bool VideoFormat16_9)
{
  if (subDevice)
     return subDevice->SetVideoFormat(VideoFormat16_9);
  cDevice::SetVideoFormat(VideoFormat16_9);
}

#if VDRVERSNUM < 20300 || defined(DEPRECATED_VIDEOSYSTEM)
eVideoSystem cDynamicDevice::GetVideoSystem(void)
{
  if (subDevice)
     return subDevice->GetVideoSystem();
  return cDevice::GetVideoSystem();
}
#endif

void cDynamicDevice::GetVideoSize(int &Width, int &Height, double &VideoAspect)
{
  if (subDevice)
     return subDevice->GetVideoSize(Width, Height, VideoAspect);
  cDevice::GetVideoSize(Width, Height, VideoAspect);
}

void cDynamicDevice::GetOsdSize(int &Width, int &Height, double &PixelAspect)
{
  if (subDevice)
     return subDevice->GetOsdSize(Width, Height, PixelAspect);
  cDevice::GetOsdSize(Width, Height, PixelAspect);
}

bool cDynamicDevice::SetPid(cPidHandle *Handle, int Type, bool On)
{
  if (subDevice)
     return subDevice->SetPid(Handle, Type, On);
  return cDevice::SetPid(Handle, Type, On);
}

int cDynamicDevice::OpenFilter(u_short Pid, u_char Tid, u_char Mask)
{
  if (subDevice)
     return subDevice->OpenFilter(Pid, Tid, Mask);
  return cDevice::OpenFilter(Pid, Tid, Mask);
}

int cDynamicDevice::ReadFilter(int Handle, void *Buffer, size_t Length)
{
  if (subDevice)
     return subDevice->ReadFilter(Handle, Buffer, Length);
  return cDevice::ReadFilter(Handle, Buffer, Length);
}

void cDynamicDevice::CloseFilter(int Handle)
{
  if (subDevice)
     return subDevice->CloseFilter(Handle);
  cDevice::CloseFilter(Handle);
}

bool cDynamicDevice::ProvidesSource(int Source) const
{
  if (!InternProvidesSource(Source))
     return false;
  if (subDevice)
     return subDevice->ProvidesSource(Source);
  return cDevice::ProvidesSource(Source);
}

bool cDynamicDevice::ProvidesTransponder(const cChannel *Channel) const
{
  if (!InternProvidesSource(Channel->Source()))
     return false;
  if (subDevice)
     return subDevice->ProvidesTransponder(Channel);
  return cDevice::ProvidesTransponder(Channel);
}

bool cDynamicDevice::ProvidesTransponderExclusively(const cChannel *Channel) const
{
  if (!InternProvidesSource(Channel->Source()))
     return false;
  if (subDevice)
     return subDevice->ProvidesTransponderExclusively(Channel);
  return cDevice::ProvidesTransponderExclusively(Channel);
}

bool cDynamicDevice::ProvidesChannel(const cChannel *Channel, int Priority, bool *NeedsDetachReceivers) const
{
  if (!InternProvidesSource(Channel->Source()))
     return false;
  if (subDevice)
     return subDevice->ProvidesChannel(Channel, Priority, NeedsDetachReceivers);
  return cDevice::ProvidesChannel(Channel, Priority, NeedsDetachReceivers);
}

int cDynamicDevice::NumProvidedSystems(void) const
{
  if (subDevice)
     return subDevice->NumProvidedSystems();
  return cDevice::NumProvidedSystems();
}

#if VDRVERSNUM > 20101
const cPositioner *cDynamicDevice::Positioner(void) const
{
  if (subDevice)
     return subDevice->Positioner();
  return cDevice::Positioner();
}
#endif

#if VDRVERSNUM > 20400
  bool cDynamicDevice::SignalStats(int &Valid, double *Strength, double *Cnr, double *BerPre, double *BerPost, double *Per, int *Status) const
{
  if (subDevice)
     return subDevice->SignalStats(Valid, Strength, Cnr, BerPre, BerPost, Per, Status);
  return cDevice::SignalStats(Valid, Strength, Cnr, BerPre, BerPost, Per, Status);
}
#endif

int cDynamicDevice::SignalStrength(void) const
{
  if (subDevice)
     return subDevice->SignalStrength();
  return cDevice::SignalStrength();
}

int cDynamicDevice::SignalQuality(void) const
{
  if (subDevice)
     return subDevice->SignalQuality();
  return cDevice::SignalQuality();
}

const cChannel *cDynamicDevice::GetCurrentlyTunedTransponder(void) const
{
  if (!IsIdle() && subDevice)
     return subDevice->GetCurrentlyTunedTransponder();
  return cDevice::GetCurrentlyTunedTransponder();
}

bool cDynamicDevice::IsTunedToTransponder(const cChannel *Channel) const
{
  if (!IsIdle() && subDevice)
     return subDevice->IsTunedToTransponder(Channel);
  return cDevice::IsTunedToTransponder(Channel);
}

bool cDynamicDevice::MaySwitchTransponder(const cChannel *Channel) const
{
  if (subDevice)
     return subDevice->MaySwitchTransponder(Channel);
  return cDevice::MaySwitchTransponder(Channel);
}

bool cDynamicDevice::SetChannelDevice(const cChannel *Channel, bool LiveView)
{
  if (subDevice)
     return subDevice->SetChannelDevice(Channel, LiveView);
  return cDevice::SetChannelDevice(Channel, LiveView);
}

bool cDynamicDevice::HasLock(int TimeoutMs) const
{
  if (subDevice)
     return subDevice->HasLock(TimeoutMs);
  return cDevice::HasLock(TimeoutMs);
}

bool cDynamicDevice::HasProgramme(void) const
{
  if (subDevice)
     return subDevice->HasProgramme();
  return cDevice::HasProgramme();
}

int cDynamicDevice::GetAudioChannelDevice(void)
{
  if (subDevice)
     return subDevice->GetAudioChannelDevice();
  return cDevice::GetAudioChannelDevice();
}

void cDynamicDevice::SetAudioChannelDevice(int AudioChannel)
{
  if (subDevice)
     return subDevice->SetAudioChannelDevice(AudioChannel);
  cDevice::SetAudioChannelDevice(AudioChannel);
}

void cDynamicDevice::SetVolumeDevice(int Volume)
{
  if (subDevice)
     return subDevice->SetVolumeDevice(Volume);
  cDevice::SetVolumeDevice(Volume);
}

void cDynamicDevice::SetDigitalAudioDevice(bool On)
{
  if (subDevice)
     return subDevice->SetDigitalAudioDevice(On);
  cDevice::SetDigitalAudioDevice(On);
}

void cDynamicDevice::SetAudioTrackDevice(eTrackType Type)
{
  if (subDevice)
     return subDevice->SetAudioTrackDevice(Type);
  cDevice::SetAudioTrackDevice(Type);
}

void cDynamicDevice::SetSubtitleTrackDevice(eTrackType Type)
{
  if (subDevice)
     return subDevice->SetSubtitleTrackDevice(Type);
  cDevice::SetSubtitleTrackDevice(Type);
}

bool cDynamicDevice::CanReplay(void) const
{
  if (subDevice)
     return subDevice->CanReplay();
  return cDevice::CanReplay();
}

bool cDynamicDevice::SetPlayMode(ePlayMode PlayMode)
{
  if (subDevice)
     return subDevice->SetPlayMode(PlayMode);
  return cDevice::SetPlayMode(PlayMode);
}

int64_t cDynamicDevice::GetSTC(void)
{
  if (subDevice)
     return subDevice->GetSTC();
  return cDevice::GetSTC();
}

bool cDynamicDevice::IsPlayingVideo(void) const
{
  if (subDevice)
     return subDevice->IsPlayingVideo();
  return cDevice::IsPlayingVideo();
}

cRect cDynamicDevice::CanScaleVideo(const cRect &Rect, int Alignment)
{
  if (subDevice)
     return subDevice->CanScaleVideo(Rect, Alignment);
  return cDevice::CanScaleVideo(Rect, Alignment);
}

void cDynamicDevice::ScaleVideo(const cRect &Rect)
{
  if (subDevice)
     return subDevice->ScaleVideo(Rect);
  return cDevice::ScaleVideo(Rect);
}

bool cDynamicDevice::HasIBPTrickSpeed(void)
{
  if (subDevice)
     return subDevice->HasIBPTrickSpeed();
  return cDevice::HasIBPTrickSpeed();
}

#if APIVERSNUM > 20102
void cDynamicDevice::TrickSpeed(int Speed, bool Forward)
{
  if (subDevice)
     return subDevice->TrickSpeed(Speed, Forward);
  cDevice::TrickSpeed(Speed, Forward);
}
#else
void cDynamicDevice::TrickSpeed(int Speed)
{
  if (subDevice)
     return subDevice->TrickSpeed(Speed);
  cDevice::TrickSpeed(Speed);
}
#endif

void cDynamicDevice::Clear(void)
{
  if (subDevice)
     return subDevice->Clear();
  cDevice::Clear();
}

void cDynamicDevice::Play(void)
{
  if (subDevice)
     return subDevice->Play();
  cDevice::Play();
}

void cDynamicDevice::Freeze(void)
{
  if (subDevice)
     return subDevice->Freeze();
  cDevice::Freeze();
}

void cDynamicDevice::Mute(void)
{
  if (subDevice)
     return subDevice->Mute();
  cDevice::Mute();
}

void cDynamicDevice::StillPicture(const uchar *Data, int Length)
{
  if (subDevice)
     return subDevice->StillPicture(Data, Length);
  cDevice::StillPicture(Data, Length);
}

bool cDynamicDevice::Poll(cPoller &Poller, int TimeoutMs)
{
  if (subDevice)
     return subDevice->Poll(Poller, TimeoutMs);
  return cDevice::Poll(Poller, TimeoutMs);
}

bool cDynamicDevice::Flush(int TimeoutMs)
{
  if (subDevice)
     return subDevice->Flush(TimeoutMs);
  return cDevice::Flush(TimeoutMs);
}

int cDynamicDevice::PlayVideo(const uchar *Data, int Length)
{
  if (subDevice)
     return subDevice->PlayVideo(Data, Length);
  return cDevice::PlayVideo(Data, Length);
}

int cDynamicDevice::PlayAudio(const uchar *Data, int Length, uchar Id)
{
  if (subDevice)
     return subDevice->PlayAudio(Data, Length, Id);
  return cDevice::PlayAudio(Data, Length, Id);
}

int cDynamicDevice::PlaySubtitle(const uchar *Data, int Length)
{
  if (subDevice)
     return subDevice->PlaySubtitle(Data, Length);
  return cDevice::PlaySubtitle(Data, Length);
}

int cDynamicDevice::PlayPesPacket(const uchar *Data, int Length, bool VideoOnly)
{
  if (subDevice)
     return subDevice->PlayPesPacket(Data, Length, VideoOnly);
  return cDevice::PlayPesPacket(Data, Length, VideoOnly);
}

int cDynamicDevice::PlayPes(const uchar *Data, int Length, bool VideoOnly)
{
  if (subDevice)
     return subDevice->PlayPes(Data, Length, VideoOnly);
  return cDevice::PlayPes(Data, Length, VideoOnly);
}

int cDynamicDevice::PlayTsVideo(const uchar *Data, int Length)
{
  if (subDevice)
     return subDevice->PlayTsVideo(Data, Length);
  return cDevice::PlayTsVideo(Data, Length);
}

int cDynamicDevice::PlayTsAudio(const uchar *Data, int Length)
{
  if (subDevice)
     return subDevice->PlayTsAudio(Data, Length);
  return cDevice::PlayTsAudio(Data, Length);
}

int cDynamicDevice::PlayTsSubtitle(const uchar *Data, int Length)
{
  if (subDevice)
     return subDevice->PlayTsSubtitle(Data, Length);
  return cDevice::PlayTsSubtitle(Data, Length);
}

int cDynamicDevice::PlayTs(const uchar *Data, int Length, bool VideoOnly)
{
  if (subDevice)
     return subDevice->PlayTs(Data, Length, VideoOnly);
  return cDevice::PlayTs(Data, Length, VideoOnly);
}

bool cDynamicDevice::Ready(void)
{
  if (subDevice)
     return subDevice->Ready();
  return cDevice::Ready();
}

bool cDynamicDevice::OpenDvr(void)
{
  lastCloseDvr = 0;
  if (subDevice) {
     getTSWatchdog = 0;
     return subDevice->OpenDvr();
     }
  return cDevice::OpenDvr();
}

void cDynamicDevice::CloseDvr(void)
{
  lastCloseDvr = time(NULL);
  if (subDevice)
     return subDevice->CloseDvr();
  cDevice::CloseDvr();
}

bool cDynamicDevice::GetTSPacket(uchar *&Data)
{
  if (subDeviceIsReady && subDevice) {
     bool r = subDevice->GetTSPacket(Data);
     if (getTSTimeout > 0) {
        if (Data == NULL) {
           if (getTSWatchdog == 0)
              getTSWatchdog = time(NULL);
           else if ((time(NULL) - getTSWatchdog) > getTSTimeout) {
              const char *d = NULL;
              if (devpath)
                 d = **devpath;
              esyslog("dynamite: device %s hasn't delivered any data for %d seconds, detaching all receivers", d, getTSTimeout);
              subDevice->DetachAllReceivers();
              cDynamicDeviceProbe::QueueDynamicDeviceCommand(ddpcDetach, *devpath);
              const char *timeoutHandlerArg = *devpath;
              if (getTSTimeoutHandlerArg)
                 timeoutHandlerArg = **getTSTimeoutHandlerArg;
              cDynamicDeviceProbe::QueueDynamicDeviceCommand(ddpcService, *cString::sprintf("dynamite-CallGetTSTimeoutHandler-v0.1 %s", timeoutHandlerArg));
              return false;
              }
           }
        else
           getTSWatchdog = 0;
        }
     return r;
     }
  return cDevice::GetTSPacket(Data);
}

void cDynamicDevice::DetachAllReceivers(void)
{
  if (subDevice)
     return subDevice->DetachAllReceivers();
  cDevice::DetachAllReceivers();
}

#ifdef YAVDR_PATCHES
//opt-44_rotor.dpatch 
bool cDynamicDevice::SendDiseqcCmd(dvb_diseqc_master_cmd cmd)
{
  if (subDevice)
     return subDevice->SendDiseqcCmd(cmd);
  return cDevice::SendDiseqcCmd(cmd);
}
#endif
