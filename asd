diff --git a/Makefile.am b/Makefile.am
index 58c5af1..be386f9 100644
--- a/Makefile.am
+++ b/Makefile.am
@@ -17,6 +17,7 @@
 # limitations under the License.
 ##########################################################################
 
+AM_CFLAGS = @CFLAGS@
 lib_LTLIBRARIES = libRDKMfrLib.la
 
 libRDKMfrLib_la_SOURCES=mfrlibs_rpi.c
@@ -27,7 +28,7 @@ endif
 libRDKMfrLib_la_CFLAGS=$(RDKMFRLIBS_CFLAGS)
 libRDKMfrLib_la_LIBADD=$(RDKMFRLIBS_LIBS)
 
-bin_PROGRAMS = mfrUtility
-mfrUtility_SOURCES = mfrlib_utility.c
-mfrUtility_LDADD = libRDKMfrLib.la
-mfrUtility_CFLAGS = $(RDKMFRLIBS_CFLAGS)
\ No newline at end of file
+bin_PROGRAMS = mfrHalUtility
+mfrHalUtility_SOURCES = mfrlib_utility.c
+mfrHalUtility_LDADD = libRDKMfrLib.la
+mfrHalUtility_CFLAGS = $(RDKMFRLIBS_CFLAGS)
\ No newline at end of file
diff --git a/configure.ac b/configure.ac
index b4d77f2..52cb4df 100644
--- a/configure.ac
+++ b/configure.ac
@@ -49,6 +49,14 @@ AC_ARG_ENABLE([thermalprotection], [--enable-thermalprotection "Enable thermal p
        esac], [thermalprotection=false])
 AM_CONDITIONAL([THERMAL_PROTECTION_ENABLED], [test x$thermalprotection = xtrue])
 
+AC_ARG_ENABLE([single-instance-lock],
+    AS_HELP_STRING([--enable-single-instance-lock], [Enable single instance lock mechanism]),
+    [enable_single_instance_lock=$enableval], [enable_single_instance_lock=no])
+
+if test "x$enable_single_instance_lock" = "xyes"; then
+    AC_DEFINE([ENABLE_SINGLE_INSTANCE_LOCK], [1], [Enable single instance lock mechanism])
+fi
+
 # Checks for library functions.
 AC_FUNC_MALLOC
 
diff --git a/mfrlibs_rpi.c b/mfrlibs_rpi.c
index 825395f..9cc019a 100644
--- a/mfrlibs_rpi.c
+++ b/mfrlibs_rpi.c
@@ -40,8 +40,11 @@
 const char defaultDescription[] = "RaspberryPi RDKV Reference Device";
 const char defaultProductClass[] = "RDKV";
 const char defaultSoftwareVersion[] = "2.0";
+static int isInitialized = 0;
 
 /* Mechanism to make this a single instance */
+#ifdef ENABLE_SINGLE_INSTANCE_LOCK
+
 #define MFRHAL_LOCK_FILE "/run/mfrhallibrary.lock"
 static int lockFd = -1;
 static int lockRefCount = 0;
@@ -85,6 +88,8 @@ int releaseLock(void)
     return -1;
 }
 
+#endif /* ENABLE_SINGLE_INSTANCE_LOCK */
+
 /* Logging function */
 void mfrlib_log(const char *format, ...)
 {
@@ -449,10 +454,16 @@ mfrError_t mfrGetSerializedData(mfrSerializedType_t param, mfrSerializedData_t *
     FILE *fp = NULL;
     mfrError_t ret = mfrERR_NONE;
 
+    if (!isInitialized) {
+        mfrlib_log("mfrGetSerializedData not initialized\n");
+        return mfrERR_NOT_INITIALIZED;
+    }
+#ifdef ENABLE_SINGLE_INSTANCE_LOCK
     if (lockFd == -1) {
-        mfrlib_log("mfrSetSerializedData not initialized\n");
+        mfrlib_log("mfrSetSerializedData not locked and initialized\n");
         return mfrERR_NOT_INITIALIZED;
     }
+#endif /* ENABLE_SINGLE_INSTANCE_LOCK */
 
     if (!data || !isValidMfrSerializedType(param)) {
         mfrlib_log("Invalid mfrSerializedType_t or data ptr is NULL\n");
@@ -751,10 +762,16 @@ mfrError_t mfrGetSerializedData(mfrSerializedType_t param, mfrSerializedData_t *
 
 mfrError_t mfrSetSerializedData( mfrSerializedType_t type,  mfrSerializedData_t *data)
 {
-    if (lockFd == -1) {
+    if (!isInitialized) {
         mfrlib_log("mfrSetSerializedData not initialized\n");
         return mfrERR_NOT_INITIALIZED;
     }
+#ifdef ENABLE_SINGLE_INSTANCE_LOCK
+    if (lockFd == -1) {
+        mfrlib_log("mfrSetSerializedData not locked and initialized\n");
+        return mfrERR_NOT_INITIALIZED;
+    }
+#endif /* ENABLE_SINGLE_INSTANCE_LOCK */
     if (!data || !isValidMfrSerializedType(type)) {
         mfrlib_log("Invalid mfrSerializedType_t or data ptr is NULL\n");
         return mfrERR_INVALID_PARAM;
@@ -764,37 +781,73 @@ mfrError_t mfrSetSerializedData( mfrSerializedType_t type,  mfrSerializedData_t
 
 mfrError_t mfrDeletePDRI()
 {
-    if (lockFd == -1) {
+    if (!isInitialized) {
         mfrlib_log("mfrDeletePDRI not initialized\n");
         return mfrERR_NOT_INITIALIZED;
     }
+#ifdef ENABLE_SINGLE_INSTANCE_LOCK
+    if (lockFd == -1) {
+        mfrlib_log("mfrDeletePDRI not locked and initialized\n");
+        return mfrERR_NOT_INITIALIZED;
+    }
+#endif /* ENABLE_SINGLE_INSTANCE_LOCK */
     return mfrERR_OPERATION_NOT_SUPPORTED;
 }
 
 mfrError_t mfrScrubAllBanks()
 {
-    if (lockFd == -1) {
+    if (!isInitialized) {
         mfrlib_log("mfrScrubAllBanks not initialized\n");
         return mfrERR_NOT_INITIALIZED;
     }
+#ifdef ENABLE_SINGLE_INSTANCE_LOCK
+    if (lockFd == -1) {
+        mfrlib_log("mfrScrubAllBanks not locked and initialized\n");
+        return mfrERR_NOT_INITIALIZED;
+    }
+#endif /* ENABLE_SINGLE_INSTANCE_LOCK */
     return mfrERR_OPERATION_NOT_SUPPORTED;
 }
 
+bool isValidMfrBLPattern(mfrBlPattern_t pattern)
+{
+    if (pattern >= mfrBL_PATTERN_NORMAL && pattern < mfrBL_PATTERN_MAX) {
+        return true;
+    }
+    return false;
+}
+
 mfrError_t mfrSetBootloaderPattern(mfrBlPattern_t pattern)
 {
-    if (lockFd == -1) {
+    if (!isInitialized) {
         mfrlib_log("mfrSetBootloaderPattern not initialized\n");
         return mfrERR_NOT_INITIALIZED;
     }
+#ifdef ENABLE_SINGLE_INSTANCE_LOCK
+    if (lockFd == -1) {
+        mfrlib_log("mfrSetBootloaderPattern not locked and initialized\n");
+        return mfrERR_NOT_INITIALIZED;
+    }
+#endif /* ENABLE_SINGLE_INSTANCE_LOCK */
+    if (!isValidMfrBLPattern(pattern)) {
+        mfrlib_log("mfrSetBootloaderPattern Invalid mfrBlPattern_t\n");
+        return mfrERR_INVALID_PARAM;
+    }
     return mfrERR_OPERATION_NOT_SUPPORTED;
 }
 
 mfrError_t mfrSetBlSplashScreen(const char *path)
 {
-    if (lockFd == -1) {
+    if (!isInitialized) {
         mfrlib_log("mfrSetBlSplashScreen not initialized\n");
         return mfrERR_NOT_INITIALIZED;
     }
+#ifdef ENABLE_SINGLE_INSTANCE_LOCK
+    if (lockFd == -1) {
+        mfrlib_log("mfrSetBlSplashScreen not locked and initialized\n");
+        return mfrERR_NOT_INITIALIZED;
+    }
+#endif /* ENABLE_SINGLE_INSTANCE_LOCK */
     if (!path) {
         mfrlib_log("mfrSetBlSplashScreen invalid input\n");
         return mfrERR_INVALID_PARAM;
@@ -804,19 +857,31 @@ mfrError_t mfrSetBlSplashScreen(const char *path)
 
 mfrError_t mfrClearBlSplashScreen(void)
 {
-    if (lockFd == -1) {
+    if (!isInitialized) {
         mfrlib_log("mfrClearBlSplashScreen not initialized\n");
         return mfrERR_NOT_INITIALIZED;
     }
+#ifdef ENABLE_SINGLE_INSTANCE_LOCK
+    if (lockFd == -1) {
+        mfrlib_log("mfrClearBlSplashScreen not locked and initialized\n");
+        return mfrERR_NOT_INITIALIZED;
+    }
+#endif /* ENABLE_SINGLE_INSTANCE_LOCK */
     return mfrERR_OPERATION_NOT_SUPPORTED;
 }
 
 mfrError_t mfrGetSecureTime(uint32_t *timeptr)
 {
-    if (lockFd == -1) {
+    if (!isInitialized) {
         mfrlib_log("mfrGetSecureTime not initialized\n");
         return mfrERR_NOT_INITIALIZED;
     }
+#ifdef ENABLE_SINGLE_INSTANCE_LOCK
+    if (lockFd == -1) {
+        mfrlib_log("mfrGetSecureTime not locked and initialized\n");
+        return mfrERR_NOT_INITIALIZED;
+    }
+#endif /* ENABLE_SINGLE_INSTANCE_LOCK */
     if (!timeptr) {
         mfrlib_log("mfrGetSecureTime invalid input\n");
         return mfrERR_INVALID_PARAM;
@@ -826,10 +891,16 @@ mfrError_t mfrGetSecureTime(uint32_t *timeptr)
 
 mfrError_t mfrSetSecureTime(uint32_t *timeptr)
 {
-    if (lockFd == -1) {
+    if (!isInitialized) {
         mfrlib_log("mfrSetSecureTime not initialized\n");
         return mfrERR_NOT_INITIALIZED;
     }
+#ifdef ENABLE_SINGLE_INSTANCE_LOCK
+    if (lockFd == -1) {
+        mfrlib_log("mfrSetSecureTime not locked and initialized\n");
+        return mfrERR_NOT_INITIALIZED;
+    }
+#endif /* ENABLE_SINGLE_INSTANCE_LOCK */
     if (!timeptr) {
         mfrlib_log("mfrSetSecureTime invalid input\n");
         return mfrERR_INVALID_PARAM;
@@ -839,10 +910,16 @@ mfrError_t mfrSetSecureTime(uint32_t *timeptr)
 
 mfrError_t mfrSetFSRflag(uint16_t *newFsrFlag)
 {
+    if (isInitialized) {
+        mfrlib_log("mfrSetFSRflag already initialized\n");
+        return mfrERR_ALREADY_INITIALIZED;
+    }
+#ifdef ENABLE_SINGLE_INSTANCE_LOCK
     if (lockFd == -1) {
-        mfrlib_log("mfrSetFSRflag not initialized\n");
+        mfrlib_log("mfrSetFSRflag not locked and initialized\n");
         return mfrERR_NOT_INITIALIZED;
     }
+#endif /* ENABLE_SINGLE_INSTANCE_LOCK */
     if (!newFsrFlag) {
         mfrlib_log("mfrSetFSRflag invalid input\n");
         return mfrERR_INVALID_PARAM;
@@ -852,10 +929,16 @@ mfrError_t mfrSetFSRflag(uint16_t *newFsrFlag)
 
 mfrError_t mfrGetFSRflag(uint16_t *newFsrFlag)
 {
+    if (isInitialized) {
+        mfrlib_log("mfrGetFSRflag already initialized\n");
+        return mfrERR_ALREADY_INITIALIZED;
+    }
+#ifdef ENABLE_SINGLE_INSTANCE_LOCK
     if (lockFd == -1) {
-        mfrlib_log("mfrGetFSRflag not initialized\n");
+        mfrlib_log("mfrGetFSRflag not locked and initialized\n");
         return mfrERR_NOT_INITIALIZED;
     }
+#endif /* ENABLE_SINGLE_INSTANCE_LOCK */
     if (!newFsrFlag) {
         mfrlib_log("mfrGetFSRflag invalid input\n");
         return mfrERR_INVALID_PARAM;
@@ -863,62 +946,126 @@ mfrError_t mfrGetFSRflag(uint16_t *newFsrFlag)
     return mfrERR_OPERATION_NOT_SUPPORTED;
 }
 
-mfrError_t mfrWriteImage(const char *str, const char *str1,
-                         mfrImageType_t imageType, mfrUpgradeStatusNotify_t upgradeStatus)
-{
-    // TODO: change FlashApp.sh logic to use mfrWriteImage
-    return mfrERR_OPERATION_NOT_SUPPORTED;
+bool isValidMfrImageType(mfrImageType_t type) {
+    if (type >= mfrIMAGE_TYPE_CDL && type < mfrIMAGE_TYPE_MAX) {
+        return true;
+    }
+    return false;
 }
 
 mfrError_t mfr_init(void)
 {
-    if (lockFd != -1) {
+    if (isInitialized) {
         mfrlib_log("mfr_init already initialized\n");
         return mfrERR_ALREADY_INITIALIZED;
     }
 
+#ifdef ENABLE_SINGLE_INSTANCE_LOCK
     if (acquireLock() == -1) {
         mfrlib_log("mfr_init acquireLock failed\n");
         return mfrERR_ALREADY_INITIALIZED;
     }
+#endif /* ENABLE_SINGLE_INSTANCE_LOCK */
+
+    isInitialized = 1;
     return mfrERR_NONE;
 }
 
 mfrError_t mfr_term(void)
 {
-    if (lockFd == -1) {
+    if (!isInitialized) {
         mfrlib_log("mfr_term not initialized\n");
         return mfrERR_NOT_INITIALIZED;
     }
 
+#ifdef ENABLE_SINGLE_INSTANCE_LOCK
     if (releaseLock() == -1) {
         mfrlib_log("mfr_term releaseLock failed\n");
         return mfrERR_NOT_INITIALIZED;
     }
+#endif /* ENABLE_SINGLE_INSTANCE_LOCK */
+
+    isInitialized = 0;
     return mfrERR_NONE;
 }
 
+mfrError_t mfrWriteImage(const char *name,  const char *path, mfrImageType_t type,  mfrUpgradeStatusNotify_t notify);
+{
+    if (!isInitialized) {
+        mfrlib_log("mfrWriteImage not initialized\n");
+        return mfrERR_NOT_INITIALIZED;
+    }
+#ifdef ENABLE_SINGLE_INSTANCE_LOCK
+    if (lockFd == -1) {
+        mfrlib_log("mfrWriteImage not locked and initialized\n");
+        return mfrERR_NOT_INITIALIZED;
+    }
+#endif /* ENABLE_SINGLE_INSTANCE_LOCK */
+
+    if (!name || !path || !isValidMfrImageType(type)) {
+        mfrlib_log("mfrWriteImage invalid input\n");
+        return mfrERR_INVALID_PARAM;
+    }
+    // TODO: change FlashApp.sh logic to use mfrWriteImage
+    return mfrERR_OPERATION_NOT_SUPPORTED;
+}
+
 /****************************** MFR WIFI APIs ********************************/
 
 WIFI_API_RESULT WIFI_GetCredentials(WIFI_DATA *pData)
 {
-   if (NULL == pData) {
+    if (!isInitialized) {
+        return WIFI_API_RESULT_NOT_INITIALIZED;
+    }
+#ifdef ENABLE_SINGLE_INSTANCE_LOCK
+    if (lockFd == -1) {
+        return WIFI_API_RESULT_NOT_INITIALIZED;
+    }
+#endif /* ENABLE_SINGLE_INSTANCE_LOCK */
+    if (NULL == pData) {
         return WIFI_API_RESULT_NULL_PARAM;
-   }
+    }
 
-   return WIFI_API_RESULT_OPERATION_NOT_SUPPORTED;
+    return WIFI_API_RESULT_OPERATION_NOT_SUPPORTED;
+}
+
+bool isValidWIFIDATAType(WIFI_DATA_TYPE type)
+{
+    if ((type > WIFI_DATA_UNKNOWN)  && (type < WIFI_DATA_MAX)) {
+        return true;
+    }
+    return false;
 }
 
 WIFI_API_RESULT WIFI_SetCredentials(WIFI_DATA *pData)
 {
+    if (!isInitialized) {
+        return WIFI_API_RESULT_NOT_INITIALIZED;
+    }
+#ifdef ENABLE_SINGLE_INSTANCE_LOCK
+    if (lockFd == -1) {
+        return WIFI_API_RESULT_NOT_INITIALIZED;
+    }
+#endif /* ENABLE_SINGLE_INSTANCE_LOCK */
     if (NULL == pData) {
         return WIFI_API_RESULT_NULL_PARAM;
     }
+    if (!isValidWIFIDATAType(pData->type)) {
+        return WIFI_API_RESULT_INVALID_PARAM;
+    }
 
     return WIFI_API_RESULT_OPERATION_NOT_SUPPORTED;
 }
 
 WIFI_API_RESULT WIFI_EraseAllData(void)
 {
+    if (!isInitialized) {
+        return WIFI_API_RESULT_NOT_INITIALIZED;
+    }
+#ifdef ENABLE_SINGLE_INSTANCE_LOCK
+    if (lockFd == -1) {
+        return WIFI_API_RESULT_NOT_INITIALIZED;
+    }
+#endif /* ENABLE_SINGLE_INSTANCE_LOCK */
     return WIFI_API_RESULT_OPERATION_NOT_SUPPORTED;
-}
\ No newline at end of file
+}
