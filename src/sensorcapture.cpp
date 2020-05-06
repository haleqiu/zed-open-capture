﻿#include "sensorcapture.hpp"

#include <sstream>
#include <cmath>              // for round
#include <unistd.h>           // for usleep, close

namespace sl_drv {

SensorCapture::SensorCapture(bool verbose )
{
    mVerbose = verbose;

    if( mVerbose )
    {
        std::string ver =
                "ZED Driver - Sensors module - Version: "
                + std::to_string(mDrvMajorVer) + "."
                + std::to_string(mDrvMinorVer) + "."
                + std::to_string(mDrvPatchVer);
        INFO_OUT( ver );
    }
}

SensorCapture::~SensorCapture()
{
    reset();
}

int SensorCapture::enumerateDevices()
{
    mSlDevPid.clear();
    mSlDevFwVer.clear();

    struct hid_device_info *devs, *cur_dev;

    if (hid_init()==-1)
        return 0;

    devs = hid_enumerate(SL_USB_VENDOR, 0x0);
    cur_dev = devs;
    while (cur_dev) {
        int fw_major = cur_dev->release_number>>8;
        int fw_minor = cur_dev->release_number&0x00FF;
        uint16_t pid = cur_dev->product_id;
        std::string sn_str = wstr2str( cur_dev->serial_number );
        int sn = std::stoi( sn_str );

        mSlDevPid[sn]=pid;
        mSlDevFwVer[sn]=cur_dev->release_number;

        if(mVerbose)
        {
            std::ostringstream smsg;

            smsg << "Device Found: " << std::endl;
            smsg << "  VID: " << std::hex << cur_dev->vendor_id << " PID: " << std::hex << cur_dev->product_id << std::endl;
            smsg << "  Path: " << cur_dev->path << std::endl;
            smsg << "  Serial_number:   " << sn_str << std::endl;
            smsg << "  Manufacturer:   " << wstr2str(cur_dev->manufacturer_string) << std::endl;
            smsg << "  Product:   " << wstr2str(cur_dev->product_string) << std::endl;
            smsg << "  Release number:   v" << fw_major << "." << fw_minor << std::endl;
            smsg << "***" << std::endl;

            INFO_OUT(smsg.str());
        }

        cur_dev = cur_dev->next;
    }

    hid_free_enumeration(devs);

    return mSlDevPid.size();
}

std::vector<int> SensorCapture::getDeviceList()
{
    if(mSlDevPid.size()==0)
        enumerateDevices();

    std::vector<int> sn_vec;

    for(std::map<int,uint16_t>::iterator it = mSlDevPid.begin(); it != mSlDevPid.end(); ++it) {
        sn_vec.push_back(it->first);
    }

    return sn_vec;
}

bool SensorCapture::init( int sn )
{
    if(mSlDevPid.size()==0)
    {
        enumerateDevices();
    }

    std::string sn_str;

    if(sn!=-1)
        sn_str = std::to_string(sn);
    else
    {
        if(mSlDevPid.size()==0)
        {
            enumerateDevices();
        }

        if(mSlDevPid.size()==0)
        {
            ERROR_OUT("No available ZED Mini or ZED2 cameras");
            return false;
        }

        sn = mSlDevPid.begin()->first;
        sn_str = std::to_string(sn);
    }

    mDevSerial = sn;

    std::wstring wide_string = std::wstring(sn_str.begin(), sn_str.end());
    const wchar_t* wsn = wide_string.c_str();

    uint16_t pid = mSlDevPid[sn];

    mDevHandle = hid_open(SL_USB_VENDOR, pid, wsn );

    if (!mDevHandle)
    {
        std::string msg = "Connection to device with sn ";
        msg += sn_str;
        msg += " failed";

        ERROR_OUT(msg);

        mDevFwVer = -1;
        mDevSerial = -1;

        return false;
    }

    if(mVerbose)
    {
        std::string msg = "Connected to device with sn ";
        msg += sn_str;

        INFO_OUT(msg);
    }

    mDevFwVer = mSlDevFwVer[sn];

    mInitialized = startCapture();

    return true;
}

void SensorCapture::getFwVersion( uint16_t& fw_major, uint16_t& fw_minor )
{
    if(mDevSerial==-1)
        return;

    uint16_t release = mSlDevFwVer[mDevSerial];

    fw_major = release>>8;
    fw_minor = release&0x00FF;
}

int SensorCapture::getSerialNumber()
{
    if(mDevSerial==-1)
        return -1;

    return mDevSerial;
}

bool SensorCapture::enableDataStream(bool enable) {
    if( !mDevHandle )
        return false;
    unsigned char buf[65];
    buf[0] = REP_ID_SENSOR_STREAM_STATUS;
    buf[1] = enable?1:0;

    int res = hid_send_feature_report(mDevHandle, buf, 2);
    if (res < 0) {
        if(mVerbose)
        {
            std::string msg = "Unable to set a feature report [SensStreamStatus] - ";
            msg += wstr2str(hid_error(mDevHandle));

            WARNING_OUT(msg);
        }

        return false;
    }

    return true;
}

bool SensorCapture::isDataStreamEnabled() {
    if( !mDevHandle ) {
        return false;
    }

    unsigned char buf[65];
    buf[0] = REP_ID_SENSOR_STREAM_STATUS;
    int res = hid_get_feature_report(mDevHandle, buf, sizeof(buf));
    if (res < 0)
    {
        std::string msg = "Unable to get a feature report [SensStreamStatus] - ";
        msg += wstr2str(hid_error(mDevHandle));

        WARNING_OUT( msg );

        return false;
    }

    if( res < static_cast<int>(sizeof(SensStreamStatus)) )
    {
        WARNING_OUT( std::string("SensStreamStatus size mismatch [REP_ID_SENSOR_STREAM_STATUS]"));
        return false;
    }

    if( buf[0] != REP_ID_SENSOR_STREAM_STATUS )
    {
        WARNING_OUT( std::string("SensStreamStatus type mismatch [REP_ID_SENSOR_STREAM_STATUS]") );

        return false;
    }

    bool enabled = (buf[1]==1);

    return enabled;
}

bool SensorCapture::startCapture()
{
    if( !enableDataStream(true) )
    {
        return false;
    }

    mGrabThread = std::thread( &SensorCapture::grabThreadFunc,this );

    return true;
}

void SensorCapture::reset()
{
    mStopCapture = true;

    if( mGrabThread.joinable() )
    {
        mGrabThread.join();
    }

    enableDataStream(false);

    if( mDevHandle ) {
        hid_close(mDevHandle);
        mDevHandle = nullptr;
    }

    if( mVerbose && mInitialized)
    {
        std::string msg = "Device closed";
        INFO_OUT( msg );
    }

    mInitialized=false;
}

void SensorCapture::grabThreadFunc()
{
    mStopCapture = false;
    mGrabRunning = false;

    mNewIMUData=false;
    mNewMagData=false;
    mNewEnvData=false;
    mNewCamTempData=false;

    // Read sensor data
    unsigned char usbBuf[65];

    int ping_data_count = 0;

    mFirstImuData = true;

    uint64_t rel_mcu_ts = 0;

    size_t size_max = 50;
    mSysTsQueue.reserve(size_max);
    mMcuTsQueue.reserve(size_max);

    while (!mStopCapture)
    {
        // ----> Keep data stream alive
        // sending a ping aboutonce per second
        // to keep the streaming alive
        if(ping_data_count>=400) {
            ping_data_count=0;
            sendPing();
        };
        ping_data_count++;
        // <---- Keep data stream alive

        mGrabRunning=true;

        // Sensor data request
        usbBuf[1]=REP_ID_SENSOR_DATA;
        int res = hid_read_timeout( mDevHandle, usbBuf, 64, 500 );

        // ----> Data received?
        if( res < static_cast<int>(sizeof(SensData)) )  {
            hid_set_nonblocking( mDevHandle, 0 );
            continue;
        }
        // <---- Data received?

        // ----> Received data are correct?
        if( usbBuf[0] != REP_ID_SENSOR_DATA )
        {
            if(mVerbose)
            {
                WARNING_OUT( std::string("REP_ID_SENSOR_DATA - Sensor Data type mismatch") );
            }

            hid_set_nonblocking( mDevHandle, 0 );
            continue;
        }
        // <---- Received data are correct?

        // Data structure static conversion
        SensData* data = (SensData*)usbBuf;

        // ----> Timestamp update
        uint64_t mcu_ts_nsec = static_cast<uint64_t>(std::round(static_cast<float>(data->timestamp)*TS_SCALE));

        if(mFirstImuData && data->imu_not_valid!=1)
        {
            mStartSysTs = getSysTs(); // Starting system timestamp
            std::cout << "SensorCapture: " << mStartSysTs << std::endl;
            mLastMcuTs = mcu_ts_nsec;
            mFirstImuData = false;
            continue;
        }

        uint64_t delta_mcu_ts_raw = mcu_ts_nsec - mLastMcuTs;

        //std::cout << "Internal MCU freq: " << 1e9/delta_mcu_ts_raw << " Hz" << std::endl;

        mLastMcuTs = mcu_ts_nsec;
        // <---- Timestamp update

        rel_mcu_ts +=  static_cast<uint64_t>(static_cast<double>(delta_mcu_ts_raw)*mNTPTsScaling);

        // mStartSysTs is synchronized to Video TS when sync is enabled using \ref VideoCapture::enableSensorSync
        uint64_t current_data_ts = (mStartSysTs-mSyncOffset) + rel_mcu_ts;

        // ----> Camera/Sensors Synchronization
        if( data->sync_capabilities != 0 ) // Synchronization active
        {
            if(mLastFrameSyncCount!=0 && (data->frame_sync!=0 || data->frame_sync_count>mLastFrameSyncCount))
            {
#if 0 // Timestamp sync debug info
                std::cout << "MCU sync information: " << std::endl;
                std::cout << " * data->frame_sync: " << (int)data->frame_sync << std::endl;
                std::cout << " * data->frame_sync_count: " << data->frame_sync_count << std::endl;
                std::cout << " * mLastFrameSyncCount: " << mLastFrameSyncCount << std::endl;
                std::cout << " * MCU timestamp scaling: " << mNTPTsScaling << std::endl;
#endif
                mLastSysSyncTs = getSteadyTs();
                mLastMcuSyncTs = mcu_ts_nsec;

                mSysTsQueue.push_back( getSteadyTs() );     // Steady host timestamp
                mMcuTsQueue.push_back( current_data_ts );   // MCU timestamp

                // Once we have enough data, calculate the drift scaling factor to appy to ntp_scaling.
                if (mSysTsQueue.size()==size_max && mMcuTsQueue.size() == size_max)
                {
                    //First and last ts
                    int first_index = 5;
                    if (mNTPAdjustedCount <= NTP_ADJUST_CT) {
                        first_index = size_max/2;
                    }

                    uint64_t first_ts_imu = mMcuTsQueue.at(first_index);
                    uint64_t last_ts_imu = mMcuTsQueue.at(mMcuTsQueue.size()-1);
                    uint64_t first_ts_cam = mSysTsQueue.at(first_index);
                    uint64_t last_ts_cam = mSysTsQueue.at(mSysTsQueue.size()-1);
                    double scale = double(last_ts_cam-first_ts_cam) / double(last_ts_imu-first_ts_imu);
                    //CLAMP
                    if (scale > 1.2) scale = 1.2;
                    if (scale < 0.8) scale = 0.8;

                    //Adjust scaling continuoulsy. No jump so that ts(n) - ts(n-1) == 800Hz
                    mNTPTsScaling*=scale;
                    //scale will be apply to the next values, so clear the vector and wait until we have enough data again
                    mMcuTsQueue.clear();
                    mSysTsQueue.clear();

                    mNTPAdjustedCount++;

                    static int64_t offset_sum = 0;
                    static int count = 0;
                    offset_sum += (static_cast<int64_t>(current_data_ts) - static_cast<int64_t>(mVideoPtr->mLastFrame.timestamp));
                    count++;

                    if(count==3)
                    {
                        int64_t offset = offset_sum/count;
                        mSyncOffset += offset;

                        std::cout << "Offset: " << offset << std::endl;
                        std::cout << "mSyncOffset: " << mSyncOffset << std::endl;

                        offset_sum = 0;
                        count=0;
                    }
                }
            }
        }
        mLastFrameSyncCount = data->frame_sync_count;
        // <---- Camera/Sensors Synchronization

        // ----> IMU data
        mIMUMutex.lock();
        mLastIMUData.sync = data->frame_sync;
        mLastIMUData.valid = data->imu_not_valid!=1;
        mLastIMUData.timestamp = current_data_ts;
        mLastIMUData.aX = data->aX*ACC_SCALE;
        mLastIMUData.aY = data->aY*ACC_SCALE;
        mLastIMUData.aZ = data->aZ*ACC_SCALE;
        mLastIMUData.gX = data->gX*GYRO_SCALE;
        mLastIMUData.gY = data->gY*GYRO_SCALE;
        mLastIMUData.gZ = data->gZ*GYRO_SCALE;
        mLastIMUData.temp = data->imu_temp*TEMP_SCALE;
        mNewIMUData = true;
        mIMUMutex.unlock();

        //std::string msg = std::to_string(mLastMAGData.timestamp);
        //INFO_OUT(msg);
        // <---- IMU data

        // ----> Magnetometer data
        if(data->mag_valid == SensMagData::NEW_VAL)
        {
            mMagMutex.lock();
            mLastMagData.valid = SensMagData::NEW_VAL;
            mLastMagData.timestamp = current_data_ts;
            mLastMagData.mY = data->mY*MAG_SCALE;
            mLastMagData.mZ = data->mZ*MAG_SCALE;
            mLastMagData.mX = data->mX*MAG_SCALE;
            mNewMagData = true;
            mMagMutex.unlock();

            //std::string msg = std::to_string(mLastMAGData.timestamp);
            //INFO_OUT(msg);
        }
        else
        {
            mLastIMUData.valid = static_cast<SensMagData::MagStatus>(data->mag_valid);
        }
        // <---- Magnetometer data

        // ----> Environmental data
        if(data->env_valid == SensEnvData::NEW_VAL)
        {
            mEnvMutex.lock();
            mLastEnvData.valid = SensEnvData::NEW_VAL;
            mLastEnvData.timestamp = current_data_ts;
            mLastEnvData.temp = data->temp*TEMP_SCALE;
            if( atLeast(mDevFwVer, ZED_2_FW::FW_3_9))
            {
                mLastEnvData.press = data->press*PRESS_SCALE_NEW;
                mLastEnvData.humid = data->humid*HUMID_SCALE_NEW;
            }
            else
            {
                mLastEnvData.press = data->press*PRESS_SCALE_OLD;
                mLastEnvData.humid = data->humid*HUMID_SCALE_OLD;
            }
            mNewEnvData = true;
            mEnvMutex.unlock();

            //std::string msg = std::to_string(mLastENVData.timestamp);
            //INFO_OUT(msg);
        }
        else
        {
            mLastIMUData.valid = static_cast<SensMagData::MagStatus>(data->mag_valid);
        }
        // <---- Environmental data

        // ----> Camera sensors temperature data
        if(data->temp_cam_left != TEMP_NOT_VALID &&
                data->temp_cam_left != TEMP_NOT_VALID &&
                data->env_valid == SensEnvData::NEW_VAL ) // Sensor temperature is linked to Environmental data acquisition at FW level
        {
            mCamTempMutex.lock();
            mLastCamTempData.valid = true;
            mLastCamTempData.timestamp = current_data_ts;
            mLastCamTempData.temp_left = data->temp_cam_left*TEMP_SCALE;
            mLastCamTempData.temp_right = data->temp_cam_right*TEMP_SCALE;
            mNewCamTempData=true;
            mCamTempMutex.unlock();

            //std::string msg = std::to_string(mLastCamTempData.timestamp);
            //INFO_OUT(msg);
        }
        else
        {
            mLastCamTempData.valid = false;
        }
        // <---- Camera sensors temperature data
    }

    mGrabRunning = false;
}

bool SensorCapture::sendPing() {
    if( !mDevHandle )
        return false;

    unsigned char buf[65];
    buf[0] = REP_ID_REQUEST_SET;
    buf[1] = RQ_CMD_PING;

    int res = hid_send_feature_report(mDevHandle, buf, 2);
    if (res < 0)
    {
        std::string msg = "Unable to send ping [REP_ID_REQUEST_SET-RQ_CMD_PING] - ";
        msg += wstr2str(hid_error(mDevHandle));

        WARNING_OUT(msg);

        return false;
    }

    return true;
}

const SensImuData* SensorCapture::getLastIMUData(uint64_t timeout_usec)
{
    // ----> Wait for a new frame
    uint64_t time_count = (timeout_usec<100?100:timeout_usec)/100;
    while( !mNewIMUData )
    {
        if(time_count==0)
        {
            return nullptr;
        }
        time_count--;
        usleep(100);
    }
    // <---- Wait for a new frame

    // Get the frame mutex
    const std::lock_guard<std::mutex> lock(mIMUMutex);
    mNewIMUData = false;
    return &mLastIMUData;
}

const SensMagData* SensorCapture::getLastMagData(uint64_t timeout_usec)
{
    // ----> Wait for a new frame
    uint64_t time_count = (timeout_usec<100?100:timeout_usec)/10;
    while( !mNewMagData )
    {
        if(time_count==0)
        {
            return nullptr;
        }
        time_count--;
        usleep(10);
    }
    // <---- Wait for a new frame

    // Get the frame mutex
    const std::lock_guard<std::mutex> lock(mMagMutex);
    mNewMagData = false;
    return &mLastMagData;
}

const SensEnvData* SensorCapture::getLastEnvData(uint64_t timeout_usec)
{
    // ----> Wait for a new frame
    uint64_t time_count = (timeout_usec<100?100:timeout_usec)/10;
    while( !mNewEnvData )
    {
        if(time_count==0)
        {
            return nullptr;
        }
        time_count--;
        usleep(10);
    }
    // <---- Wait for a new frame

    // Get the frame mutex
    const std::lock_guard<std::mutex> lock(mEnvMutex);
    mNewEnvData = false;
    return &mLastEnvData;
}

const SensCamTempData* SensorCapture::getLastCamTempData(uint64_t timeout_usec)
{
    // ----> Wait for a new frame
    uint64_t time_count = (timeout_usec<100?100:timeout_usec)/10;
    while( !mNewCamTempData )
    {
        if(time_count==0)
        {
            return nullptr;
        }
        time_count--;
        usleep(10);
    }
    // <---- Wait for a new frame

    // Get the frame mutex
    const std::lock_guard<std::mutex> lock(mCamTempMutex);
    mNewCamTempData = false;
    return &mLastCamTempData;
}

}
