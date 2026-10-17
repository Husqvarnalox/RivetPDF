// SPDX-License-Identifier: MPL-2.0
#pragma once

// PRIVATE header: a minimal, ABI-compatible subset of the OASIS PKCS#11 v2.40
// ("Cryptoki") interface, written from the specification. Rivet does not
// vendor the OASIS headers; the structure layouts and numeric constants below
// were cross-checked (sizeof / offsetof / every constant) against the
// reference headers shipped with CryptoPro CSP
// (/opt/cprocsp/include/cppkcs11/pkcs11.h, pkcs11t.h, pkcs11f.h) with a
// throw-away program that was compiled against both. Those headers are
// proprietary and are read for comparison only, never copied.
//
// Rules: this header is included only by the PKCS#11 provider sources and by
// the test-only mock module. No PKCS#11 type may appear in a public header.
//
// ABI notes: CK_ULONG is `unsigned long` (64 bit on LP64 Unix, 32 bit on
// Windows). Unix uses natural alignment; Windows uses 1-byte packing as the
// specification mandates.

#include <cstddef>
#include <cstdint>

#if defined(_WIN32)
#pragma pack(push, 1)
#endif

namespace rivet::crypto::p11 {

using CK_BYTE = unsigned char;
using CK_CHAR = CK_BYTE;
using CK_UTF8CHAR = CK_BYTE;
using CK_BBOOL = CK_BYTE;
using CK_ULONG = unsigned long;
using CK_LONG = long;
using CK_FLAGS = CK_ULONG;
using CK_RV = CK_ULONG;
using CK_SLOT_ID = CK_ULONG;
using CK_SESSION_HANDLE = CK_ULONG;
using CK_OBJECT_HANDLE = CK_ULONG;
using CK_USER_TYPE = CK_ULONG;
using CK_STATE = CK_ULONG;
using CK_OBJECT_CLASS = CK_ULONG;
using CK_ATTRIBUTE_TYPE = CK_ULONG;
using CK_MECHANISM_TYPE = CK_ULONG;
using CK_KEY_TYPE = CK_ULONG;
using CK_CERTIFICATE_TYPE = CK_ULONG;
using CK_RSA_PKCS_MGF_TYPE = CK_ULONG;
using CK_NOTIFICATION = CK_ULONG;

inline constexpr CK_ULONG CK_UNAVAILABLE_INFORMATION = ~CK_ULONG{0};
inline constexpr CK_BBOOL CK_TRUE = 1;
inline constexpr CK_BBOOL CK_FALSE = 0;

struct CK_VERSION {
    CK_BYTE major;
    CK_BYTE minor;
};

struct CK_INFO {
    CK_VERSION cryptokiVersion;
    CK_UTF8CHAR manufacturerID[32];
    CK_FLAGS flags;
    CK_UTF8CHAR libraryDescription[32];
    CK_VERSION libraryVersion;
};

struct CK_SLOT_INFO {
    CK_UTF8CHAR slotDescription[64];
    CK_UTF8CHAR manufacturerID[32];
    CK_FLAGS flags;
    CK_VERSION hardwareVersion;
    CK_VERSION firmwareVersion;
};

struct CK_TOKEN_INFO {
    CK_UTF8CHAR label[32];
    CK_UTF8CHAR manufacturerID[32];
    CK_UTF8CHAR model[16];
    CK_CHAR serialNumber[16];
    CK_FLAGS flags;
    CK_ULONG ulMaxSessionCount;
    CK_ULONG ulSessionCount;
    CK_ULONG ulMaxRwSessionCount;
    CK_ULONG ulRwSessionCount;
    CK_ULONG ulMaxPinLen;
    CK_ULONG ulMinPinLen;
    CK_ULONG ulTotalPublicMemory;
    CK_ULONG ulFreePublicMemory;
    CK_ULONG ulTotalPrivateMemory;
    CK_ULONG ulFreePrivateMemory;
    CK_VERSION hardwareVersion;
    CK_VERSION firmwareVersion;
    CK_CHAR utcTime[16];
};

struct CK_SESSION_INFO {
    CK_SLOT_ID slotID;
    CK_STATE state;
    CK_FLAGS flags;
    CK_ULONG ulDeviceError;
};

struct CK_ATTRIBUTE {
    CK_ATTRIBUTE_TYPE type;
    void* pValue;
    CK_ULONG ulValueLen;
};

struct CK_MECHANISM {
    CK_MECHANISM_TYPE mechanism;
    void* pParameter;
    CK_ULONG ulParameterLen;
};

struct CK_MECHANISM_INFO {
    CK_ULONG ulMinKeySize;
    CK_ULONG ulMaxKeySize;
    CK_FLAGS flags;
};

struct CK_RSA_PKCS_PSS_PARAMS {
    CK_MECHANISM_TYPE hashAlg;
    CK_RSA_PKCS_MGF_TYPE mgf;
    CK_ULONG sLen;
};

struct CK_C_INITIALIZE_ARGS {
    void* CreateMutex;
    void* DestroyMutex;
    void* LockMutex;
    void* UnlockMutex;
    CK_FLAGS flags;
    void* pReserved;
};

// ---- return values (CKR_*) -----------------------------------------------
inline constexpr CK_RV CKR_OK = 0x00000000;
inline constexpr CK_RV CKR_CANCEL = 0x00000001;
inline constexpr CK_RV CKR_HOST_MEMORY = 0x00000002;
inline constexpr CK_RV CKR_SLOT_ID_INVALID = 0x00000003;
inline constexpr CK_RV CKR_GENERAL_ERROR = 0x00000005;
inline constexpr CK_RV CKR_FUNCTION_FAILED = 0x00000006;
inline constexpr CK_RV CKR_ARGUMENTS_BAD = 0x00000007;
inline constexpr CK_RV CKR_ATTRIBUTE_SENSITIVE = 0x00000011;
inline constexpr CK_RV CKR_ATTRIBUTE_TYPE_INVALID = 0x00000012;
inline constexpr CK_RV CKR_DEVICE_ERROR = 0x00000030;
inline constexpr CK_RV CKR_DEVICE_MEMORY = 0x00000031;
inline constexpr CK_RV CKR_DEVICE_REMOVED = 0x00000032;
inline constexpr CK_RV CKR_FUNCTION_NOT_SUPPORTED = 0x00000054;
inline constexpr CK_RV CKR_KEY_HANDLE_INVALID = 0x00000060;
inline constexpr CK_RV CKR_MECHANISM_INVALID = 0x00000070;
inline constexpr CK_RV CKR_MECHANISM_PARAM_INVALID = 0x00000071;
inline constexpr CK_RV CKR_PIN_INCORRECT = 0x000000A0;
inline constexpr CK_RV CKR_PIN_INVALID = 0x000000A1;
inline constexpr CK_RV CKR_PIN_LEN_RANGE = 0x000000A2;
inline constexpr CK_RV CKR_PIN_EXPIRED = 0x000000A3;
inline constexpr CK_RV CKR_PIN_LOCKED = 0x000000A4;
inline constexpr CK_RV CKR_SESSION_CLOSED = 0x000000B0;
inline constexpr CK_RV CKR_SESSION_COUNT = 0x000000B1;
inline constexpr CK_RV CKR_SESSION_HANDLE_INVALID = 0x000000B3;
inline constexpr CK_RV CKR_SESSION_READ_ONLY = 0x000000B5;
inline constexpr CK_RV CKR_TOKEN_NOT_PRESENT = 0x000000E0;
inline constexpr CK_RV CKR_TOKEN_NOT_RECOGNIZED = 0x000000E1;
inline constexpr CK_RV CKR_USER_ALREADY_LOGGED_IN = 0x00000100;
inline constexpr CK_RV CKR_USER_NOT_LOGGED_IN = 0x00000101;
inline constexpr CK_RV CKR_USER_PIN_NOT_INITIALIZED = 0x00000102;
inline constexpr CK_RV CKR_USER_ANOTHER_ALREADY_LOGGED_IN = 0x00000104;
inline constexpr CK_RV CKR_BUFFER_TOO_SMALL = 0x00000150;
inline constexpr CK_RV CKR_CRYPTOKI_NOT_INITIALIZED = 0x00000190;
inline constexpr CK_RV CKR_CRYPTOKI_ALREADY_INITIALIZED = 0x00000191;

// ---- attributes (CKA_*) ----------------------------------------------------
inline constexpr CK_ATTRIBUTE_TYPE CKA_CLASS = 0x00000000;
inline constexpr CK_ATTRIBUTE_TYPE CKA_TOKEN = 0x00000001;
inline constexpr CK_ATTRIBUTE_TYPE CKA_PRIVATE = 0x00000002;
inline constexpr CK_ATTRIBUTE_TYPE CKA_LABEL = 0x00000003;
inline constexpr CK_ATTRIBUTE_TYPE CKA_VALUE = 0x00000011;
inline constexpr CK_ATTRIBUTE_TYPE CKA_CERTIFICATE_TYPE = 0x00000080;
inline constexpr CK_ATTRIBUTE_TYPE CKA_ISSUER = 0x00000081;
inline constexpr CK_ATTRIBUTE_TYPE CKA_SERIAL_NUMBER = 0x00000082;
inline constexpr CK_ATTRIBUTE_TYPE CKA_KEY_TYPE = 0x00000100;
inline constexpr CK_ATTRIBUTE_TYPE CKA_SUBJECT = 0x00000101;
inline constexpr CK_ATTRIBUTE_TYPE CKA_ID = 0x00000102;
inline constexpr CK_ATTRIBUTE_TYPE CKA_SIGN = 0x00000108;
inline constexpr CK_ATTRIBUTE_TYPE CKA_MODULUS = 0x00000120;
inline constexpr CK_ATTRIBUTE_TYPE CKA_MODULUS_BITS = 0x00000121;
inline constexpr CK_ATTRIBUTE_TYPE CKA_PUBLIC_EXPONENT = 0x00000122;
inline constexpr CK_ATTRIBUTE_TYPE CKA_EC_PARAMS = 0x00000180;
inline constexpr CK_ATTRIBUTE_TYPE CKA_EC_POINT = 0x00000181;

// ---- object classes, key / certificate types ------------------------------
inline constexpr CK_OBJECT_CLASS CKO_CERTIFICATE = 0x00000001;
inline constexpr CK_OBJECT_CLASS CKO_PUBLIC_KEY = 0x00000002;
inline constexpr CK_OBJECT_CLASS CKO_PRIVATE_KEY = 0x00000003;
inline constexpr CK_KEY_TYPE CKK_RSA = 0x00000000;
inline constexpr CK_KEY_TYPE CKK_EC = 0x00000003;
inline constexpr CK_KEY_TYPE CKK_GOSTR3410 = 0x00000030;
inline constexpr CK_CERTIFICATE_TYPE CKC_X_509 = 0x00000000;

// ---- flags ------------------------------------------------------------------
inline constexpr CK_FLAGS CKF_TOKEN_PRESENT = 0x00000001;           // slot
inline constexpr CK_FLAGS CKF_RNG = 0x00000001;                     // token
inline constexpr CK_FLAGS CKF_WRITE_PROTECTED = 0x00000002;
inline constexpr CK_FLAGS CKF_LOGIN_REQUIRED = 0x00000004;
inline constexpr CK_FLAGS CKF_USER_PIN_INITIALIZED = 0x00000008;
inline constexpr CK_FLAGS CKF_PROTECTED_AUTHENTICATION_PATH = 0x00000100;
inline constexpr CK_FLAGS CKF_TOKEN_INITIALIZED = 0x00000400;
inline constexpr CK_FLAGS CKF_USER_PIN_COUNT_LOW = 0x00010000;
inline constexpr CK_FLAGS CKF_USER_PIN_FINAL_TRY = 0x00020000;
inline constexpr CK_FLAGS CKF_USER_PIN_LOCKED = 0x00040000;
inline constexpr CK_FLAGS CKF_USER_PIN_TO_BE_CHANGED = 0x00080000;
inline constexpr CK_FLAGS CKF_RW_SESSION = 0x00000002;              // session
inline constexpr CK_FLAGS CKF_SERIAL_SESSION = 0x00000004;
inline constexpr CK_FLAGS CKF_OS_LOCKING_OK = 0x00000002;      // C_Initialize
inline constexpr CK_FLAGS CKF_SIGN = 0x00000800;                    // mechanism

inline constexpr CK_USER_TYPE CKU_SO = 0;
inline constexpr CK_USER_TYPE CKU_USER = 1;

inline constexpr CK_STATE CKS_RO_PUBLIC_SESSION = 0;
inline constexpr CK_STATE CKS_RO_USER_FUNCTIONS = 1;
inline constexpr CK_STATE CKS_RW_PUBLIC_SESSION = 2;
inline constexpr CK_STATE CKS_RW_USER_FUNCTIONS = 3;
inline constexpr CK_STATE CKS_RW_SO_FUNCTIONS = 4;

// ---- mechanisms (CKM_*) -----------------------------------------------------
inline constexpr CK_MECHANISM_TYPE CKM_RSA_PKCS = 0x00000001;
inline constexpr CK_MECHANISM_TYPE CKM_RSA_PKCS_PSS = 0x0000000D;
inline constexpr CK_MECHANISM_TYPE CKM_SHA256_RSA_PKCS = 0x00000040;
inline constexpr CK_MECHANISM_TYPE CKM_SHA384_RSA_PKCS = 0x00000041;
inline constexpr CK_MECHANISM_TYPE CKM_SHA512_RSA_PKCS = 0x00000042;
inline constexpr CK_MECHANISM_TYPE CKM_SHA256_RSA_PKCS_PSS = 0x00000043;
inline constexpr CK_MECHANISM_TYPE CKM_SHA384_RSA_PKCS_PSS = 0x00000044;
inline constexpr CK_MECHANISM_TYPE CKM_SHA512_RSA_PKCS_PSS = 0x00000045;
inline constexpr CK_MECHANISM_TYPE CKM_SHA256 = 0x00000250;
inline constexpr CK_MECHANISM_TYPE CKM_SHA384 = 0x00000260;
inline constexpr CK_MECHANISM_TYPE CKM_SHA512 = 0x00000270;
inline constexpr CK_MECHANISM_TYPE CKM_ECDSA = 0x00001041;
inline constexpr CK_MECHANISM_TYPE CKM_ECDSA_SHA1 = 0x00001042;
inline constexpr CK_MECHANISM_TYPE CKM_ECDSA_SHA224 = 0x00001043;
inline constexpr CK_MECHANISM_TYPE CKM_ECDSA_SHA256 = 0x00001044;
inline constexpr CK_MECHANISM_TYPE CKM_ECDSA_SHA384 = 0x00001045;
inline constexpr CK_MECHANISM_TYPE CKM_ECDSA_SHA512 = 0x00001046;
inline constexpr CK_RSA_PKCS_MGF_TYPE CKG_MGF1_SHA256 = 0x00000002;
inline constexpr CK_RSA_PKCS_MGF_TYPE CKG_MGF1_SHA384 = 0x00000003;
inline constexpr CK_RSA_PKCS_MGF_TYPE CKG_MGF1_SHA512 = 0x00000004;

// ---- function list ----------------------------------------------------------
// Entries Rivet calls are typed; the others are `void*` placeholders that keep
// the 68 slots of the specification's order (a module may leave any of them
// NULL, so every call site checks for NULL).
using CK_C_Initialize = CK_RV (*)(void* pInitArgs);
using CK_C_Finalize = CK_RV (*)(void* pReserved);
using CK_C_GetSlotList = CK_RV (*)(CK_BBOOL tokenPresent, CK_SLOT_ID* pSlotList, CK_ULONG* pulCount);
using CK_C_GetSlotInfo = CK_RV (*)(CK_SLOT_ID slotID, CK_SLOT_INFO* pInfo);
using CK_C_GetTokenInfo = CK_RV (*)(CK_SLOT_ID slotID, CK_TOKEN_INFO* pInfo);
using CK_C_GetMechanismList = CK_RV (*)(CK_SLOT_ID slotID, CK_MECHANISM_TYPE* pMechanismList, CK_ULONG* pulCount);
using CK_C_OpenSession = CK_RV (*)(CK_SLOT_ID slotID, CK_FLAGS flags, void* pApplication, void* Notify,
                                   CK_SESSION_HANDLE* phSession);
using CK_C_CloseSession = CK_RV (*)(CK_SESSION_HANDLE hSession);
using CK_C_GetSessionInfo = CK_RV (*)(CK_SESSION_HANDLE hSession, CK_SESSION_INFO* pInfo);
using CK_C_Login = CK_RV (*)(CK_SESSION_HANDLE hSession, CK_USER_TYPE userType, CK_UTF8CHAR* pPin, CK_ULONG ulPinLen);
using CK_C_GetAttributeValue = CK_RV (*)(CK_SESSION_HANDLE hSession, CK_OBJECT_HANDLE hObject, CK_ATTRIBUTE* pTemplate,
                                         CK_ULONG ulCount);
using CK_C_FindObjectsInit = CK_RV (*)(CK_SESSION_HANDLE hSession, CK_ATTRIBUTE* pTemplate, CK_ULONG ulCount);
using CK_C_FindObjects = CK_RV (*)(CK_SESSION_HANDLE hSession, CK_OBJECT_HANDLE* phObject, CK_ULONG ulMaxObjectCount,
                                   CK_ULONG* pulObjectCount);
using CK_C_FindObjectsFinal = CK_RV (*)(CK_SESSION_HANDLE hSession);
using CK_C_DigestInit = CK_RV (*)(CK_SESSION_HANDLE hSession, CK_MECHANISM* pMechanism);
using CK_C_Digest = CK_RV (*)(CK_SESSION_HANDLE hSession, CK_BYTE* pData, CK_ULONG ulDataLen, CK_BYTE* pDigest,
                              CK_ULONG* pulDigestLen);
using CK_C_SignInit = CK_RV (*)(CK_SESSION_HANDLE hSession, CK_MECHANISM* pMechanism, CK_OBJECT_HANDLE hKey);
using CK_C_Sign = CK_RV (*)(CK_SESSION_HANDLE hSession, CK_BYTE* pData, CK_ULONG ulDataLen, CK_BYTE* pSignature,
                            CK_ULONG* pulSignatureLen);

struct CK_FUNCTION_LIST {
    CK_VERSION version;
    CK_C_Initialize C_Initialize;
    CK_C_Finalize C_Finalize;
    void* C_GetInfo;
    void* C_GetFunctionList;
    CK_C_GetSlotList C_GetSlotList;
    CK_C_GetSlotInfo C_GetSlotInfo;
    CK_C_GetTokenInfo C_GetTokenInfo;
    CK_C_GetMechanismList C_GetMechanismList;
    void* C_GetMechanismInfo;
    void* C_InitToken;
    void* C_InitPIN;
    void* C_SetPIN;
    CK_C_OpenSession C_OpenSession;
    CK_C_CloseSession C_CloseSession;
    void* C_CloseAllSessions;
    CK_C_GetSessionInfo C_GetSessionInfo;
    void* C_GetOperationState;
    void* C_SetOperationState;
    CK_C_Login C_Login;
    void* C_Logout;
    void* C_CreateObject;
    void* C_CopyObject;
    void* C_DestroyObject;
    void* C_GetObjectSize;
    CK_C_GetAttributeValue C_GetAttributeValue;
    void* C_SetAttributeValue;
    CK_C_FindObjectsInit C_FindObjectsInit;
    CK_C_FindObjects C_FindObjects;
    CK_C_FindObjectsFinal C_FindObjectsFinal;
    void* C_EncryptInit;
    void* C_Encrypt;
    void* C_EncryptUpdate;
    void* C_EncryptFinal;
    void* C_DecryptInit;
    void* C_Decrypt;
    void* C_DecryptUpdate;
    void* C_DecryptFinal;
    CK_C_DigestInit C_DigestInit;
    CK_C_Digest C_Digest;
    void* C_DigestUpdate;
    void* C_DigestKey;
    void* C_DigestFinal;
    CK_C_SignInit C_SignInit;
    CK_C_Sign C_Sign;
    void* C_SignUpdate;
    void* C_SignFinal;
    void* C_SignRecoverInit;
    void* C_SignRecover;
    void* C_VerifyInit;
    void* C_Verify;
    void* C_VerifyUpdate;
    void* C_VerifyFinal;
    void* C_VerifyRecoverInit;
    void* C_VerifyRecover;
    void* C_DigestEncryptUpdate;
    void* C_DecryptDigestUpdate;
    void* C_SignEncryptUpdate;
    void* C_DecryptVerifyUpdate;
    void* C_GenerateKey;
    void* C_GenerateKeyPair;
    void* C_WrapKey;
    void* C_UnwrapKey;
    void* C_DeriveKey;
    void* C_SeedRandom;
    void* C_GenerateRandom;
    void* C_GetFunctionStatus;
    void* C_CancelFunction;
    void* C_WaitForSlotEvent;
};

using CK_C_GetFunctionList = CK_RV (*)(CK_FUNCTION_LIST** ppFunctionList);

} // namespace rivet::crypto::p11

#if defined(_WIN32)
#pragma pack(pop)
#endif

// Layout guard for LP64 Unix (macOS / Linux, natural alignment). The numbers
// were produced by compiling the same sizeof / offsetof list against this
// header and against /opt/cprocsp/include/cppkcs11/pkcs11t.h (CryptoPro,
// PKCS#11 v2.20-era layout, identical to v2.40 for these structures): all 11
// structures, all 68 function-list slots and all 100 constants above matched.
#if !defined(_WIN32) && defined(__LP64__)
namespace rivet::crypto::p11 {
static_assert(sizeof(CK_ULONG) == 8);
static_assert(sizeof(CK_VERSION) == 2);
static_assert(sizeof(CK_INFO) == 88 && offsetof(CK_INFO, flags) == 40 && offsetof(CK_INFO, libraryVersion) == 80);
static_assert(sizeof(CK_SLOT_INFO) == 112 && offsetof(CK_SLOT_INFO, flags) == 96 &&
              offsetof(CK_SLOT_INFO, firmwareVersion) == 106);
static_assert(sizeof(CK_TOKEN_INFO) == 208 && offsetof(CK_TOKEN_INFO, serialNumber) == 80 &&
              offsetof(CK_TOKEN_INFO, flags) == 96 && offsetof(CK_TOKEN_INFO, ulFreePrivateMemory) == 176 &&
              offsetof(CK_TOKEN_INFO, hardwareVersion) == 184 && offsetof(CK_TOKEN_INFO, utcTime) == 188);
static_assert(sizeof(CK_SESSION_INFO) == 32 && offsetof(CK_SESSION_INFO, state) == 8);
static_assert(sizeof(CK_ATTRIBUTE) == 24 && offsetof(CK_ATTRIBUTE, pValue) == 8 &&
              offsetof(CK_ATTRIBUTE, ulValueLen) == 16);
static_assert(sizeof(CK_MECHANISM) == 24 && offsetof(CK_MECHANISM, pParameter) == 8);
static_assert(sizeof(CK_MECHANISM_INFO) == 24 && offsetof(CK_MECHANISM_INFO, flags) == 16);
static_assert(sizeof(CK_RSA_PKCS_PSS_PARAMS) == 24 && offsetof(CK_RSA_PKCS_PSS_PARAMS, sLen) == 16);
static_assert(sizeof(CK_C_INITIALIZE_ARGS) == 48 && offsetof(CK_C_INITIALIZE_ARGS, flags) == 32);
static_assert(sizeof(CK_FUNCTION_LIST) == 552 && offsetof(CK_FUNCTION_LIST, C_Initialize) == 8 &&
              offsetof(CK_FUNCTION_LIST, C_GetSlotList) == 40 && offsetof(CK_FUNCTION_LIST, C_OpenSession) == 104 &&
              offsetof(CK_FUNCTION_LIST, C_Login) == 152 && offsetof(CK_FUNCTION_LIST, C_GetAttributeValue) == 200 &&
              offsetof(CK_FUNCTION_LIST, C_FindObjectsFinal) == 232 && offsetof(CK_FUNCTION_LIST, C_DigestInit) == 304 &&
              offsetof(CK_FUNCTION_LIST, C_SignInit) == 344 && offsetof(CK_FUNCTION_LIST, C_Sign) == 352 &&
              offsetof(CK_FUNCTION_LIST, C_WaitForSlotEvent) == 544);
} // namespace rivet::crypto::p11
#endif
