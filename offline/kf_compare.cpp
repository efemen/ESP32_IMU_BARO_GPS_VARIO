#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <math.h>
#include "common.h"
#include "config.h"
#include "imu.h"
#include "flashlog.h"
#include "ringbuf.h"
#include "kalmanfilter2.h"
#include "kalmanfilter3.h"
#include "kalmanfilter4.h"

// First edit config.h to set the desired analysis options
// g++ -o kf_compare kf_compare.cpp kalmanfilter2.cpp kalmanfilter3.cpp kalmanfilter4.cpp imu.cpp ringbuf.cpp -lm
// ./kf_compare ./datalog > result.txt

int main(int argc, char* argv[]) {
   if (argc != 2) {
      printf("usage : %s <ibg binary file>\r\n", argv[0]);
      return -1;
      }
	FILE* fp = fopen(argv[1], "rb");
	if (fp == NULL) {
		printf("error opening %s", argv[1]);
		return(-1);
		}

   IBG_HDR hdr;
   I_RECORD imu;
   B_RECORD baro;
   G_RECORD gps;
   int kalmanFilterInitialized = 0;
   float gx,gy,gz,ax,ay,az,mx, my, mz; 
   float baroAltCm, velNorth, velEast, velDown;
   float iirClimbrateCps, kfClimbrateCps,kfAltitudeCm;
   float glideRatio = 1.0f;

	while (!feof(fp)) {	
      hdr.magic = 0;
		int hdrSize = fread(&hdr,1, sizeof(IBG_HDR), fp);
      int numRecordBytes = 0;
		if (hdrSize == sizeof(IBG_HDR)) {
         if (hdr.magic == FLASHLOG_IBG_MAGIC) {
      		int imuSize = fread(&imu,1, sizeof(I_RECORD), fp);
            if (imuSize == sizeof(I_RECORD)) {
			      gx = DEG2RAD(imu.gxNEDdps);
			      gy = DEG2RAD(imu.gyNEDdps);
			      gz = DEG2RAD(imu.gzNEDdps);
			      ax = imu.axNEDmG;
			      ay = imu.ayNEDmG;
			      az = imu.azNEDmG;
			      mx = imu.mxNED;
			      my = imu.myNED;
			      mz = imu.mzNED;
               float asqd = ax*ax + ay*ay + az*az;
               // constrain use of accelerometer data to the window [0.75G, 1.25G] for determining
               // the orientation quaternion
		         int useAccel = ((asqd > 562500.0f) && (asqd < 1562500.0f)) ? 1 : 0;	
               int useMag = 1;
		         imu_mahonyAHRSupdate9DOF(useAccel, useMag, 0.002,gx,gy,gz,ax,ay,az,mx,my,mz);
		         float yawDeg,pitchDeg,rollDeg;
         	   imu_quaternion2YawPitchRoll(q0,q1,q2,q3, (float*)&yawDeg, (float*)&pitchDeg, (float*)&rollDeg);
			     // printf("%f, %f, %f, %f, %f, %f, %f\r\n", q0, q1, q2, q3, yawDeg, pitchDeg, rollDeg);
               float gravityCompensatedAccel = imu_gravityCompensatedAccel(ax,ay,az, q0, q1, q2, q3);
               ringbuf_addSample(gravityCompensatedAccel); 
               } 
            if (hdr.baroFlags || hdr.gpsFlags) {
      		   int baroSize = fread(&baro,1, sizeof(B_RECORD), fp);
                  
               if ((baroSize == sizeof(B_RECORD)) && hdr.baroFlags) {
                  baroAltCm = (float) baro.heightMSLcm;
                  if (kalmanFilterInitialized == 0) {
#if (USE_KF2 == 1)
                     kalmanFilter2_configure(120.0f, 90000.0f, baroAltCm, 0.0f);
#elif (USE_KF3 == 1)
                     kalmanFilter3_configure(120.0f, 90000.0f, 0.005f, baroAltCm, 0.0f);
#elif (USE_KF4 == 1)
                     kalmanFilter4_configure(120.0f, 50000.0f, 90000.0f, 0.005f, baroAltCm, 0.0f, 0.0f);
#endif
                     kalmanFilterInitialized = 1;
                     }
#if (USE_KF2 == 1)
                  kalmanFilter2_predict(90000.0f, 0.02f);
                  kalmanFilter2_update(baroAltCm, (float*)&kfAltitudeCm, (float*)&kfClimbrateCps);

#elif (USE_KF3 == 1)                     
				      float zAccelAverage = ringbuf_averageOldestSamples(10); 
                  kalmanFilter3_predict(zAccelAverage, 0.02f);
                  kalmanFilter3_update(baroAltCm, (float*)&kfAltitudeCm, (float*)&kfClimbrateCps);

#elif (USE_KF4 == 1)
				      float zAccelAverage = ringbuf_averageNewestSamples(10); 
                  kalmanFilter4_predict(0.02f);
                  kalmanFilter4_update(baroAltCm, zAccelAverage, (float*)&kfAltitudeCm, (float*)&kfClimbrateCps);
#endif

#if (LOG_INPUT_OUTPUT == 1)
                  if (kalmanFilterInitialized) {
                     printf("%.1f %.1f %.1f\r\n", baroAltCm, kfAltitudeCm, kfClimbrateCps);
                     }
#endif                    
                  // use damped climbrate for lcd display and for glide ratio computation
                  iirClimbrateCps = iirClimbrateCps*0.9f + 0.1f*kfClimbrateCps; 
                  } 
               }
            if (hdr.gpsFlags) {
      		   int gpsSize = fread(&gps,1, sizeof(G_RECORD), fp);
               if (gpsSize == sizeof(G_RECORD)) {
                  velNorth = (float) gps.velNorthmmps;
                  velEast = (float) gps.velEastmmps;
                  velDown = (float) gps.velDownmmps;
                  float velHorz = (float)sqrt((double)velNorth*velNorth + (double)velEast*velEast);
                  if (velDown > 0.0f) {
                     float glide = velHorz/velDown;
                     glideRatio = glideRatio*0.97f + glide*0.03f;
                     }
                  velHorz /= 10.0f; // in cm/s
                  float velkph = (velHorz*3600.0f)/100000.0f;
                  } 
               }
            }
         else {
            printf("\r\nError : magic not found\r\n");
            break;
            }
         }
      else {
        // printf("\r\nDid not read hdr len bytes\r\n");
         break;
         }
      }
   fclose(fp);
   }

