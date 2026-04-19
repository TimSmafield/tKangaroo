/*
 * This file is part of the BSGS distribution (https://github.com/JeanLucPons/Kangaroo).
 * Copyright (c) 2020 Jean Luc PONS.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 3.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
*/

#include "Kangaroo.h"
#include <fstream>
#include "SECPK1/IntGroup.h"
#include "Timer.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>
#define _USE_MATH_DEFINES
#include <math.h>
#include <algorithm>
#include <sstream>
#include <vector>
#ifdef WIN64
#include <io.h>
#endif
#ifndef WIN64
#include <pthread.h>
#endif
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/rsa.h>

using namespace std;

#define safe_delete_array(x) if(x) {delete[] x;x=NULL;}
volatile sig_atomic_t g_stopRequested = 0;

namespace {

struct ScopedFile {
  FILE* handle;
  ScopedFile(FILE* file = NULL) : handle(file) {}
  ~ScopedFile() {
    if(handle != NULL)
      fclose(handle);
  }
};

struct ScopedEvpCipherCtx {
  EVP_CIPHER_CTX* handle;
  ScopedEvpCipherCtx() : handle(EVP_CIPHER_CTX_new()) {}
  ~ScopedEvpCipherCtx() {
    if(handle != NULL)
      EVP_CIPHER_CTX_free(handle);
  }
};

struct ScopedEvpPkeyCtx {
  EVP_PKEY_CTX* handle;
  explicit ScopedEvpPkeyCtx(EVP_PKEY* pkey) : handle(EVP_PKEY_CTX_new(pkey,NULL)) {}
  ~ScopedEvpPkeyCtx() {
    if(handle != NULL)
      EVP_PKEY_CTX_free(handle);
  }
};

struct ScopedEvpPkey {
  EVP_PKEY* handle;
  ScopedEvpPkey() : handle(NULL) {}
  ~ScopedEvpPkey() {
    if(handle != NULL)
      EVP_PKEY_free(handle);
  }
};

struct ScopedRsa {
  RSA* handle;
  ScopedRsa() : handle(NULL) {}
  ~ScopedRsa() {
    if(handle != NULL)
      RSA_free(handle);
  }
};

string JsonEscape(const string& value) {
  string escaped;
  escaped.reserve(value.size());
  for(size_t i = 0; i < value.size(); i++) {
    unsigned char c = (unsigned char)value[i];
    switch(c) {
      case '\\':
        escaped += "\\\\";
        break;
      case '"':
        escaped += "\\\"";
        break;
      case '\b':
        escaped += "\\b";
        break;
      case '\f':
        escaped += "\\f";
        break;
      case '\n':
        escaped += "\\n";
        break;
      case '\r':
        escaped += "\\r";
        break;
      case '\t':
        escaped += "\\t";
        break;
      default:
        if(c < 0x20) {
          char tmp[7];
          sprintf(tmp,"\\u%04x",c);
          escaped += tmp;
        } else {
          escaped.push_back((char)c);
        }
        break;
    }
  }
  return escaped;
}

string Base64Encode(const unsigned char* data,size_t len) {
  static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  if(len == 0)
    return "";
  string out;
  out.reserve(((len + 2) / 3) * 4);
  for(size_t i = 0; i < len; i += 3) {
    unsigned int chunk = ((unsigned int)data[i]) << 16;
    if(i + 1 < len)
      chunk |= ((unsigned int)data[i + 1]) << 8;
    if(i + 2 < len)
      chunk |= (unsigned int)data[i + 2];
    out.push_back(alphabet[(chunk >> 18) & 0x3F]);
    out.push_back(alphabet[(chunk >> 12) & 0x3F]);
    out.push_back((i + 1 < len) ? alphabet[(chunk >> 6) & 0x3F] : '=');
    out.push_back((i + 2 < len) ? alphabet[chunk & 0x3F] : '=');
  }
  return out;
}

string GetDirectoryName(const string& path) {
  size_t pos = path.find_last_of("/\\");
  if(pos == string::npos)
    return "";
  if(pos == 0)
    return path.substr(0,1);
  return path.substr(0,pos);
}

string JoinPath(const string& dir,const string& fileName) {
  if(dir.length() == 0 || dir == ".")
    return fileName;
  char last = dir[dir.length() - 1];
  if(last == '/' || last == '\\')
    return dir + fileName;
  return dir + "/" + fileName;
}

string GetUtcTimestamp() {
  time_t now = time(NULL);
  struct tm utcTime;
#ifdef WIN64
  gmtime_s(&utcTime,&now);
#else
  gmtime_r(&now,&utcTime);
#endif
  char buffer[32];
  strftime(buffer,sizeof(buffer),"%Y-%m-%dT%H:%M:%SZ",&utcTime);
  return string(buffer);
}

string GetOpenSSLError(const string& fallback) {
  unsigned long code = ERR_get_error();
  if(code == 0)
    return fallback;
  char buffer[256];
  ERR_error_string_n(code,buffer,sizeof(buffer));
  return string(buffer);
}

bool LoadRecipientPublicKey(const string& publicKeyFile,ScopedEvpPkey& recipientKey,string* errorMessage) {
  ScopedFile keyFile(fopen(publicKeyFile.c_str(),"rb"));
  if(keyFile.handle == NULL) {
    *errorMessage = "Cannot read public key file";
    return false;
  }

  ScopedRsa rsa;
  rsa.handle = PEM_read_RSA_PUBKEY(keyFile.handle,NULL,NULL,NULL);
  if(rsa.handle == NULL) {
    *errorMessage = GetOpenSSLError("Public key file is not a valid PEM RSA public key");
    return false;
  }

  recipientKey.handle = EVP_PKEY_new();
  if(recipientKey.handle == NULL) {
    *errorMessage = GetOpenSSLError("Failed to allocate EVP_PKEY");
    return false;
  }
  if(EVP_PKEY_assign_RSA(recipientKey.handle,rsa.handle) != 1) {
    *errorMessage = GetOpenSSLError("Failed to assign RSA public key");
    return false;
  }
  rsa.handle = NULL;
  return true;
}

bool EncryptPayloadWithOpenSSL(const string& publicKeyFile,const string& payload,string& artifact,string& errorMessage) {
  ScopedEvpPkey recipientKey;
  if(!LoadRecipientPublicKey(publicKeyFile,recipientKey,&errorMessage))
    return false;

  vector<unsigned char> aesKey(32);
  vector<unsigned char> iv(12);
  if(RAND_bytes(&aesKey[0],(int)aesKey.size()) != 1 ||
     RAND_bytes(&iv[0],(int)iv.size()) != 1) {
    errorMessage = GetOpenSSLError("Failed to generate encryption randomness");
    return false;
  }

  vector<unsigned char> plaintext(payload.begin(),payload.end());
  vector<unsigned char> ciphertext(plaintext.size() + 16);
  vector<unsigned char> tag(16);
  int outLen = 0;
  int totalLen = 0;
  ScopedEvpCipherCtx cipherCtx;
  if(cipherCtx.handle == NULL) {
    errorMessage = GetOpenSSLError("Failed to allocate cipher context");
    return false;
  }
  if(EVP_EncryptInit_ex(cipherCtx.handle,EVP_aes_256_gcm(),NULL,NULL,NULL) != 1 ||
     EVP_CIPHER_CTX_ctrl(cipherCtx.handle,EVP_CTRL_GCM_SET_IVLEN,(int)iv.size(),NULL) != 1 ||
     EVP_EncryptInit_ex(cipherCtx.handle,NULL,NULL,&aesKey[0],&iv[0]) != 1 ||
     EVP_EncryptUpdate(cipherCtx.handle,
                       plaintext.empty() ? NULL : &ciphertext[0],&outLen,
                       plaintext.empty() ? NULL : &plaintext[0],(int)plaintext.size()) != 1) {
    errorMessage = GetOpenSSLError("Failed to encrypt payload with AES-256-GCM");
    return false;
  }
  totalLen = outLen;
  if(EVP_EncryptFinal_ex(cipherCtx.handle,&ciphertext[0] + totalLen,&outLen) != 1 ||
     EVP_CIPHER_CTX_ctrl(cipherCtx.handle,EVP_CTRL_GCM_GET_TAG,(int)tag.size(),&tag[0]) != 1) {
    errorMessage = GetOpenSSLError("Failed to finalize AES-256-GCM encryption");
    return false;
  }
  totalLen += outLen;
  ciphertext.resize((size_t)totalLen);

  ScopedEvpPkeyCtx pkeyCtx(recipientKey.handle);
  if(pkeyCtx.handle == NULL) {
    errorMessage = GetOpenSSLError("Failed to allocate public key context");
    return false;
  }
  if(EVP_PKEY_encrypt_init(pkeyCtx.handle) != 1 ||
     EVP_PKEY_CTX_set_rsa_padding(pkeyCtx.handle,RSA_PKCS1_OAEP_PADDING) <= 0 ||
     EVP_PKEY_CTX_set_rsa_oaep_md(pkeyCtx.handle,EVP_sha256()) <= 0 ||
     EVP_PKEY_CTX_set_rsa_mgf1_md(pkeyCtx.handle,EVP_sha256()) <= 0) {
    errorMessage = GetOpenSSLError("Failed to initialize RSA-OAEP encryption");
    return false;
  }
  size_t wrappedKeyLen = 0;
  if(EVP_PKEY_encrypt(pkeyCtx.handle,NULL,&wrappedKeyLen,&aesKey[0],aesKey.size()) != 1) {
    errorMessage = GetOpenSSLError("Failed to size RSA-encrypted AES key");
    return false;
  }
  vector<unsigned char> wrappedKey(wrappedKeyLen);
  if(EVP_PKEY_encrypt(pkeyCtx.handle,&wrappedKey[0],&wrappedKeyLen,&aesKey[0],aesKey.size()) != 1) {
    errorMessage = GetOpenSSLError("Failed to encrypt AES key with RSA-OAEP");
    return false;
  }
  wrappedKey.resize(wrappedKeyLen);

  ostringstream out;
  out << "{\n"
      << "  \"version\": 1,\n"
      << "  \"rsa_encrypted_key\": \"" << Base64Encode(&wrappedKey[0],wrappedKey.size()) << "\",\n"
      << "  \"iv\": \"" << Base64Encode(&iv[0],iv.size()) << "\",\n"
      << "  \"ciphertext\": \"" << Base64Encode(ciphertext.empty() ? (const unsigned char*)"" : &ciphertext[0],ciphertext.size()) << "\",\n"
      << "  \"tag\": \"" << Base64Encode(&tag[0],tag.size()) << "\",\n"
      << "  \"key_encryption\": \"RSA-OAEP-SHA256\",\n"
      << "  \"content_encryption\": \"AES-256-GCM\"\n"
      << "}\n";
  artifact = out.str();
  return true;
}

bool ValidateWritableOutputPath(const string& outputFile,uint32_t pid,string* errorMessage) {
  string outputDir = GetDirectoryName(outputFile);
  if(outputDir.length() == 0)
    outputDir = ".";

  ostringstream tempName;
  tempName << ".kangaroo-output-check-" << pid << "-" << (long long)time(NULL) << ".tmp";
  string tempPath = JoinPath(outputDir,tempName.str());

  ScopedFile tempFile(fopen(tempPath.c_str(),"wb"));
  if(tempFile.handle == NULL) {
    *errorMessage = string("Output directory is not writable: ") + strerror(errno);
    return false;
  }
  fclose(tempFile.handle);
  tempFile.handle = NULL;
  if(remove(tempPath.c_str()) != 0) {
    *errorMessage = string("Failed to remove validation temp file: ") + strerror(errno);
    return false;
  }
  return true;
}

}  // namespace

static void solver_sig_handler(int signo) {
  if(signo == SIGINT || signo == SIGTERM) {
    g_stopRequested = 1;
  }
}

RunResult Kangaroo::GetRunResult() const {
  return runResult;
}

void Kangaroo::SetRunResult(RunResult result) {
  runResult = result;
}

// ----------------------------------------------------------------------------

Kangaroo::Kangaroo(Secp256K1 *secp,int32_t initDPSize,bool useGpu,string &workFile,string &iWorkFile,uint32_t savePeriod,bool saveKangaroo,bool saveKangarooByServer,
                   double maxStep,int wtimeout,int port,int ntimeout,string serverIp,string outputFile,string publicKeyFile,bool cleanupOnFound,bool splitWorkfile) {

  this->secp = secp;
  this->initDPSize = initDPSize;
  this->useGpu = useGpu;
  this->offsetCount = 0;
  this->offsetTime = 0.0;
  this->workFile = workFile;
  this->saveWorkPeriod = savePeriod;
  this->inputFile = iWorkFile;
  this->nbLoadedWalk = 0;
  this->clientMode = serverIp.length() > 0;
  this->saveKangarooByServer = this->clientMode && saveKangarooByServer;
  this->saveKangaroo = saveKangaroo || this->saveKangarooByServer;
  this->fRead = NULL;
  this->maxStep = maxStep;
  this->wtimeout = wtimeout;
  this->port = port;
  this->ntimeout = ntimeout;
  this->serverIp = serverIp;
  this->outputFile = outputFile;
  this->publicKeyFile = publicKeyFile;
  this->configFilePath = "";
  this->hostInfo = NULL;
  this->endOfSearch = false;
  this->saveRequest = false;
  this->connectedClient = 0;
  this->totalRW = 0;
  this->collisionInSameHerd = 0;
  this->keyIdx = 0;
  this->splitWorkfile = splitWorkfile;
  this->cleanupOnFound = cleanupOnFound;
  this->pid = Timer::getPID();
  this->runResult = RESULT_FATAL_ERROR;

  CPU_GRP_SIZE = 1024;

  // Init mutex
#ifdef WIN64
  ghMutex = CreateMutex(NULL,FALSE,NULL);
  saveMutex = CreateMutex(NULL,FALSE,NULL);
#else
  pthread_mutex_init(&ghMutex, NULL);
  pthread_mutex_init(&saveMutex, NULL);
  signal(SIGPIPE, SIG_IGN);
#endif

}

// ----------------------------------------------------------------------------

bool Kangaroo::ParseConfigFile(std::string &fileName) {

  // In client mode, config come from the server
  if(clientMode) {
    configFilePath = "";
    return true;
  }

  configFilePath = fileName;

  // Check file
  FILE *fp = fopen(fileName.c_str(),"rb");
  if(fp == NULL) {
    ::printf("Error: Cannot open %s %s\n",fileName.c_str(),strerror(errno));
    return false;
  }
  fclose(fp);

  // Get lines
  vector<string> lines;
  int nbLine = 0;
  string line;
  ifstream inFile(fileName);
  while(getline(inFile,line)) {

    // Remove ending \r\n
    int l = (int)line.length() - 1;
    while(l >= 0 && isspace(line.at(l))) {
      line.pop_back();
      l--;
    }

    if(line.length() > 0) {
      lines.push_back(line);
      nbLine++;
    }

  }

  if(lines.size()<3) {
    ::printf("Error: %s not enough arguments\n",fileName.c_str());
    return false;
  }

  rangeStart.SetBase16((char *)lines[0].c_str());
  rangeEnd.SetBase16((char *)lines[1].c_str());
  for(int i=2;i<(int)lines.size();i++) {
    
    Point p;
    bool isCompressed;
    if( !secp->ParsePublicKeyHex(lines[i],p,isCompressed) ) {
      ::printf("%s, error line %d: %s\n",fileName.c_str(),i,lines[i].c_str());
      return false;
    }
    keysToSearch.push_back(p);

  }

  ::printf("Start:%s\n",rangeStart.GetBase16().c_str());
  ::printf("Stop :%s\n",rangeEnd.GetBase16().c_str());
  ::printf("Keys :%d\n",(int)keysToSearch.size());

  return true;

}

bool Kangaroo::ValidateEncryptedOutputConfiguration() {
  if(publicKeyFile.length() == 0)
    return true;

  if(outputFile.length() == 0) {
    printf("Error: --pubkey requires -o <path> for the encrypted artifact\n");
    SetRunResult(RESULT_OUTPUT_ERROR);
    return false;
  }

  ScopedFile publicKey(fopen(publicKeyFile.c_str(),"rb"));
  if(publicKey.handle == NULL) {
    printf("Error: Cannot open public key file %s: %s\n",publicKeyFile.c_str(),strerror(errno));
    SetRunResult(RESULT_OUTPUT_ERROR);
    return false;
  }

  ScopedEvpPkey publicKeyHandle;
  string errorMessage;
  if(!LoadRecipientPublicKey(publicKeyFile,publicKeyHandle,&errorMessage)) {
    printf("Error: %s\n",errorMessage.c_str());
    SetRunResult(RESULT_OUTPUT_ERROR);
    return false;
  }

  if(!ValidateWritableOutputPath(outputFile,pid,&errorMessage)) {
    printf("Error: %s\n",errorMessage.c_str());
    SetRunResult(RESULT_OUTPUT_ERROR);
    return false;
  }

  return true;
}

bool Kangaroo::ValidateOutputConfiguration() {
  return ValidateEncryptedOutputConfiguration();
}

// ----------------------------------------------------------------------------

bool Kangaroo::IsDP(uint64_t x) {

  return (x & dMask) == 0;

}

void Kangaroo::SetDP(int size) {

  // Mask for distinguised point
  dpSize = size;
  if(dpSize == 0) {
    dMask = 0;
  } else {
    if(dpSize > 64) dpSize = 64;
    dMask = (1ULL << (64 - dpSize)) - 1;
    dMask = ~dMask;
  }

#ifdef WIN64
  ::printf("DP size: %d [0x%016I64X]\n",dpSize,dMask);
#else
  ::printf("DP size: %d [0x%" PRIx64 "]\n",dpSize,dMask);
#endif

}

// ----------------------------------------------------------------------------

bool Kangaroo::WritePlaintextResult(Int *pk,char sInfo,int sType) {

  FILE* f = stdout;
  bool needToClose = false;

  if(outputFile.length() > 0) {
    f = fopen(outputFile.c_str(),"a");
    if(f == NULL) {
      printf("Cannot open %s for writing\n",outputFile.c_str());
      f = stdout;
    }
    else {
      needToClose = true;
    }
  }

  if(!needToClose)
    ::printf("\n");

  Point PR = secp->ComputePublicKey(pk);

  ::fprintf(f,"Key#%2d [%d%c]Pub:  0x%s \n",keyIdx,sType,sInfo,secp->GetPublicKeyHex(true,keysToSearch[keyIdx]).c_str());
  if(PR.equals(keysToSearch[keyIdx])) {
    ::fprintf(f,"       Priv: 0x%s \n",pk->GetBase16().c_str());
  } else {
    ::fprintf(f,"       Failed !\n");
    if(needToClose)
      fclose(f);
    return false;
  }

  if(needToClose)
    fclose(f);

  return true;

}

string Kangaroo::BuildEncryptedPayload(Int *pk,char sInfo,int sType) const {
  Point matchedPoint = keysToSearch[keyIdx];
  Point recoveredPoint = secp->ComputePublicKey(pk);
  Int privateKeyCopy(*pk);
  Int rangeStartCopy(rangeStart);
  Int rangeEndCopy(rangeEnd);

  string matchedPubHex = secp->GetPublicKeyHex(true,matchedPoint);
  string recoveredPubHex = secp->GetPublicKeyHex(true,recoveredPoint);
  string recoveredPrivHex = privateKeyCopy.GetBase16();
  string rangeStartHex = rangeStartCopy.GetBase16();
  string rangeEndHex = rangeEndCopy.GetBase16();

  ostringstream payload;
  payload << "{\n"
          << "  \"schema_version\": 1,\n"
          << "  \"artifact_type\": \"kangaroo-recovered-result\",\n"
          << "  \"curve_name\": \"secp256k1\",\n"
          << "  \"timestamp_utc\": \"" << JsonEscape(GetUtcTimestamp()) << "\",\n"
          << "  \"key_index\": " << keyIdx << ",\n"
          << "  \"match_type\": \"" << sInfo << "\",\n"
          << "  \"solution_type\": " << sType << ",\n"
          << "  \"matched_public_key\": \"0x" << JsonEscape(matchedPubHex) << "\",\n"
          << "  \"recovered_public_key\": \"0x" << JsonEscape(recoveredPubHex) << "\",\n"
          << "  \"recovered_private_key\": \"0x" << JsonEscape(recoveredPrivHex) << "\",\n"
          << "  \"search_range_start\": \"0x" << JsonEscape(rangeStartHex) << "\",\n"
          << "  \"search_range_end\": \"0x" << JsonEscape(rangeEndHex) << "\"\n"
          << "}\n";
  return payload.str();
}

bool Kangaroo::WriteEncryptedResult(Int *pk,char sInfo,int sType) {
  string artifact;
  string errorMessage;
  string payload = BuildEncryptedPayload(pk,sInfo,sType);

  if(!EncryptPayloadWithOpenSSL(publicKeyFile,payload,artifact,errorMessage)) {
    printf("\nEncrypted output failed: %s\n",errorMessage.c_str());
    return false;
  }

  FILE* out = fopen(outputFile.c_str(),"wb");
  if(out == NULL) {
    printf("\nCannot open %s for writing encrypted result: %s\n",outputFile.c_str(),strerror(errno));
    return false;
  }

  size_t written = fwrite(artifact.data(),1,artifact.size(),out);
  bool closeOk = fclose(out) == 0;
  if(written != artifact.size() || !closeOk) {
    printf("\nFailed to write encrypted result to %s\n",outputFile.c_str());
    return false;
  }

  if(cleanupOnFound) {
    BestEffortCleanupOnFound();
  }

  printf("\nKey#%2d [%d%c] encrypted result written to %s\n",keyIdx,sType,sInfo,outputFile.c_str());
  return true;
}

bool Kangaroo::BestEffortScrubAndDeleteFile(const std::string& path,std::string* warning) {
  if(path.length() == 0)
    return true;

  FILE* file = fopen(path.c_str(),"rb+");
  bool scrubOk = true;
  bool missing = false;

  if(file != NULL) {
    if(fseek(file,0,SEEK_END) != 0) {
      scrubOk = false;
      *warning = "failed to seek for overwrite";
    } else {
      long size = ftell(file);
      if(size < 0) {
        scrubOk = false;
        *warning = "failed to determine file size for overwrite";
      } else if(fseek(file,0,SEEK_SET) != 0) {
        scrubOk = false;
        *warning = "failed to rewind for overwrite";
      } else {
        vector<unsigned char> zeros(4096,0);
        long remaining = size;
        while(remaining > 0) {
          size_t chunk = remaining > (long)zeros.size() ? zeros.size() : (size_t)remaining;
          if(fwrite(&zeros[0],1,chunk,file) != chunk) {
            scrubOk = false;
            *warning = "failed during overwrite pass";
            break;
          }
          remaining -= (long)chunk;
        }
        if(scrubOk) {
          if(fflush(file) != 0) {
            scrubOk = false;
            *warning = "failed to flush overwrite pass";
          } else {
#ifdef WIN64
            if(_commit(_fileno(file)) != 0) {
#else
            if(fsync(fileno(file)) != 0) {
#endif
              scrubOk = false;
              *warning = "failed to sync overwrite pass";
            }
          }
        }
      }
    }
    if(fclose(file) != 0 && scrubOk) {
      scrubOk = false;
      *warning = "failed to close overwritten file";
    }
  } else if(errno == ENOENT) {
    missing = true;
  } else {
    scrubOk = false;
    *warning = string("failed to open for overwrite: ") + strerror(errno);
  }

  if(remove(path.c_str()) != 0) {
    if(errno != ENOENT) {
      if(scrubOk && !missing)
        *warning = string("failed to delete file: ") + strerror(errno);
      else if(warning->length() == 0)
        *warning = string("failed to delete file: ") + strerror(errno);
      return false;
    }
  }

  if(missing)
    return true;

  return scrubOk;
}

bool Kangaroo::BestEffortCleanupOnFound() {
  bool ok = true;
  vector<string> targets;

  if(workFile.length() > 0)
    targets.push_back(workFile);
  if(configFilePath.length() > 0)
    targets.push_back(configFilePath);

  sort(targets.begin(),targets.end());
  targets.erase(unique(targets.begin(),targets.end()),targets.end());

  for(size_t i = 0; i < targets.size(); i++) {
    string warning;
    if(!BestEffortScrubAndDeleteFile(targets[i],&warning)) {
      ok = false;
      printf("Cleanup warning: %s (%s)\n",targets[i].c_str(),warning.c_str());
    }
  }

  return ok;
}

bool Kangaroo::Output(Int *pk,char sInfo,int sType) {

  Point PR = secp->ComputePublicKey(pk);
  if(!PR.equals(keysToSearch[keyIdx])) {
    if(publicKeyFile.length() == 0) {
      ::printf("\n");
      ::printf("Key#%2d [%d%c]Pub:  0x%s \n",keyIdx,sType,sInfo,secp->GetPublicKeyHex(true,keysToSearch[keyIdx]).c_str());
      ::printf("       Failed !\n");
    }
    return false;
  }

  bool ok = publicKeyFile.length() > 0 ? WriteEncryptedResult(pk,sInfo,sType) : WritePlaintextResult(pk,sInfo,sType);
  SetRunResult(ok ? RESULT_KEY_FOUND : RESULT_OUTPUT_ERROR);
  return true;

}

// ----------------------------------------------------------------------------

bool  Kangaroo::CheckKey(Int d1,Int d2,uint8_t type) {

  // Resolve equivalence collision

  if(type & 0x1)
    d1.ModNegK1order();
  if(type & 0x2)
    d2.ModNegK1order();

  Int pk(&d1);
  pk.ModAddK1order(&d2);

  Point P = secp->ComputePublicKey(&pk);

  if(P.equals(keyToSearch)) {
    // Key solved    
#ifdef USE_SYMMETRY
    pk.ModAddK1order(&rangeWidthDiv2);
#endif
    pk.ModAddK1order(&rangeStart);    
    return Output(&pk,'N',type);
  }

  if(P.equals(keyToSearchNeg)) {
    // Key solved
    pk.ModNegK1order();
#ifdef USE_SYMMETRY
    pk.ModAddK1order(&rangeWidthDiv2);
#endif
    pk.ModAddK1order(&rangeStart);
    return Output(&pk,'S',type);
  }

  return false;

}

bool Kangaroo::CollisionCheck(Int* d1,uint32_t type1,Int* d2,uint32_t type2) {


  if(type1 == type2) {

    // Collision inside the same herd
    return false;

  } else {

    Int Td;
    Int Wd;

    if(type1 == TAME) {
      Td.Set(d1);
      Wd.Set(d2);
    }  else {
      Td.Set(d2);
      Wd.Set(d1);
    }

    endOfSearch = CheckKey(Td,Wd,0) || CheckKey(Td,Wd,1) || CheckKey(Td,Wd,2) || CheckKey(Td,Wd,3);

    if(!endOfSearch) {

      // Should not happen, reset the kangaroo
      ::printf("\n Unexpected wrong collision, reset kangaroo !\n");
      if((int64_t)(Td.bits64[3])<0) {
        Td.ModNegK1order();
        ::printf("Found: Td-%s\n",Td.GetBase16().c_str());
      } else {
        ::printf("Found: Td %s\n",Td.GetBase16().c_str());
      }
      if((int64_t)(Wd.bits64[3])<0) {
        Wd.ModNegK1order();
        ::printf("Found: Wd-%s\n",Wd.GetBase16().c_str());
      } else {
        ::printf("Found: Wd %s\n",Wd.GetBase16().c_str());
      }
      return false;

    }

    if(GetRunResult() == RESULT_OUTPUT_ERROR)
      return true;

  }

  return true;

}

// ----------------------------------------------------------------------------

bool Kangaroo::AddToTable(Int *pos,Int *dist,uint32_t kType) {

  int addStatus = hashTable.Add(pos,dist,kType);
  if(addStatus== ADD_COLLISION)
    return CollisionCheck(&hashTable.kDist,hashTable.kType,dist,kType);

  return addStatus == ADD_OK;

}

bool Kangaroo::AddToTable(uint64_t h,int128_t *x,int128_t *d) {

  int addStatus = hashTable.Add(h,x,d);
  if(addStatus== ADD_COLLISION) {

    Int dist;
    uint32_t kType;
    HashTable::CalcDistAndType(*d,&dist,&kType);
    return CollisionCheck(&hashTable.kDist,hashTable.kType,&dist,kType);

  }

  return addStatus == ADD_OK;

}

// ----------------------------------------------------------------------------

void Kangaroo::SolveKeyCPU(TH_PARAM *ph) {

  vector<ITEM> dps;
  vector<uint32_t> deadWalkers;
  double lastSent = 0;

  // Global init
  int thId = ph->threadId;

  // Create Kangaroos
  ph->nbKangaroo = CPU_GRP_SIZE;

#ifdef USE_SYMMETRY
  ph->symClass = new uint64_t[CPU_GRP_SIZE];
  for(int i = 0; i<CPU_GRP_SIZE; i++) ph->symClass[i] = 0;
#endif

  IntGroup *grp = new IntGroup(CPU_GRP_SIZE);
  Int *dx = new Int[CPU_GRP_SIZE];

  if(ph->px==NULL) {

    // Create Kangaroos, if not already loaded
    ph->px = new Int[CPU_GRP_SIZE];
    ph->py = new Int[CPU_GRP_SIZE];
    ph->distance = new Int[CPU_GRP_SIZE];
    CreateHerd(CPU_GRP_SIZE,ph->px,ph->py,ph->distance,TAME);

  }

  if(keyIdx==0)
    ::printf("SolveKeyCPU Thread %d: %d kangaroos\n",ph->threadId,CPU_GRP_SIZE);

  ph->hasStarted = true;

  // Using Affine coord
  Int dy;
  Int rx;
  Int ry;
  Int _s;
  Int _p;

  while(!endOfSearch) {

    // Random walk

    for(int g = 0; g < CPU_GRP_SIZE; g++) {

#ifdef USE_SYMMETRY
      uint64_t jmp = ph->px[g].bits64[0] % (NB_JUMP/2) + (NB_JUMP / 2) * ph->symClass[g];
#else
      uint64_t jmp = ph->px[g].bits64[0] % NB_JUMP;
#endif

      Int *p1x = &jumpPointx[jmp];
      Int *p2x = &ph->px[g];
      dx[g].ModSub(p2x,p1x);

    }

    grp->Set(dx);
    grp->ModInv();

    for(int g = 0; g < CPU_GRP_SIZE; g++) {

#ifdef USE_SYMMETRY
      uint64_t jmp = ph->px[g].bits64[0] % (NB_JUMP / 2) + (NB_JUMP / 2) * ph->symClass[g];
#else
      uint64_t jmp = ph->px[g].bits64[0] % NB_JUMP;
#endif

      Int *p1x = &jumpPointx[jmp];
      Int *p1y = &jumpPointy[jmp];
      Int *p2x = &ph->px[g];
      Int *p2y = &ph->py[g];

      dy.ModSub(p2y,p1y);
      _s.ModMulK1(&dy,&dx[g]);
      _p.ModSquareK1(&_s);

      rx.ModSub(&_p,p1x);
      rx.ModSub(p2x);

      ry.ModSub(p2x,&rx);
      ry.ModMulK1(&_s);
      ry.ModSub(p2y);

      ph->distance[g].ModAddK1order(&jumpDistance[jmp]);

#ifdef USE_SYMMETRY
      // Equivalence symmetry class switch
      if( ry.ModPositiveK1() ) {
        ph->distance[g].ModNegK1order();
        ph->symClass[g] = !ph->symClass[g];
      }
#endif

      ph->px[g].Set(&rx);
      ph->py[g].Set(&ry);

    }

    if( clientMode ) {

      // Send DP to server
      for(int g = 0; g < CPU_GRP_SIZE; g++) {
        if(IsDP(ph->px[g].bits64[3])) {
          ITEM it;
          it.x.Set(&ph->px[g]);
          it.d.Set(&ph->distance[g]);
          it.kIdx = g;
          dps.push_back(it);
        }
      }

      double now = Timer::get_tick();
      if( now-lastSent > SEND_PERIOD ) {
        LOCK(ghMutex);
        SendToServer(dps,ph->threadId,0xFFFF,&deadWalkers);
        for(size_t i = 0; i < deadWalkers.size(); i++) {
          uint32_t kIdx = deadWalkers[i];
          if(kIdx >= (uint32_t)CPU_GRP_SIZE)
            continue;
          CreateHerd(1,&ph->px[kIdx],&ph->py[kIdx],&ph->distance[kIdx],kIdx % 2,false);
        }
        UNLOCK(ghMutex);
        lastSent = now;
      }

      if(!endOfSearch) counters[thId] += CPU_GRP_SIZE;

    } else {

      // Add to table and collision check
      for(int g = 0; g < CPU_GRP_SIZE && !endOfSearch; g++) {

        if(IsDP(ph->px[g].bits64[3])) {
          LOCK(ghMutex);
          if(!endOfSearch) {

            if(!AddToTable(&ph->px[g],&ph->distance[g],g % 2)) {
              // Collision inside the same herd
              // We need to reset the kangaroo
              CreateHerd(1,&ph->px[g],&ph->py[g],&ph->distance[g],g % 2,false);
              collisionInSameHerd++;
            }

          }
          UNLOCK(ghMutex);
        }

        if(!endOfSearch) counters[thId] ++;

      }

    }

    // Save request
    if(saveRequest && !endOfSearch) {
      ph->isWaiting = true;
      LOCK(saveMutex);
      ph->isWaiting = false;
      UNLOCK(saveMutex);
    }

  }

  // Free
  delete grp;
  delete[] dx;
  safe_delete_array(ph->px);
  safe_delete_array(ph->py);
  safe_delete_array(ph->distance);
#ifdef USE_SYMMETRY
  safe_delete_array(ph->symClass);
#endif

  ph->isRunning = false;

}

// ----------------------------------------------------------------------------

void Kangaroo::SolveKeyGPU(TH_PARAM *ph) {

  double lastSent = 0;

  // Global init
  int thId = ph->threadId;

#ifdef WITHGPU

  vector<ITEM> dps;
  vector<ITEM> gpuFound;
  vector<uint32_t> deadWalkers;
  GPUEngine *gpu;

  gpu = new GPUEngine(ph->gridSizeX,ph->gridSizeY,ph->gpuId,65536 * 2);

  if(keyIdx == 0)
    ::printf("GPU: %s (%.1f MB used)\n",gpu->deviceName.c_str(),gpu->GetMemory() / 1048576.0);

  double t0 = Timer::get_tick();


  if( ph->px==NULL ) {
    if(keyIdx == 0)
      ::printf("SolveKeyGPU Thread GPU#%d: creating kangaroos...\n",ph->gpuId);
    // Create Kangaroos, if not already loaded
    uint64_t nbThread = gpu->GetNbThread();
    ph->px = new Int[ph->nbKangaroo];
    ph->py = new Int[ph->nbKangaroo];
    ph->distance = new Int[ph->nbKangaroo];

    for(uint64_t i = 0; i<nbThread; i++) {
      CreateHerd(GPU_GRP_SIZE,&(ph->px[i*GPU_GRP_SIZE]),
                              &(ph->py[i*GPU_GRP_SIZE]),
                              &(ph->distance[i*GPU_GRP_SIZE]),
                              TAME);
    }
  }

#ifdef USE_SYMMETRY
  gpu->SetWildOffset(&rangeWidthDiv4);
#else
  gpu->SetWildOffset(&rangeWidthDiv2);
#endif
  gpu->SetParams(dMask,jumpDistance,jumpPointx,jumpPointy);
  gpu->SetKangaroos(ph->px,ph->py,ph->distance);

  if(workFile.length()==0 || !saveKangaroo) {
    // No need to get back kangaroo, free memory
    safe_delete_array(ph->px);
    safe_delete_array(ph->py);
    safe_delete_array(ph->distance);
  }

  gpu->callKernel();

  double t1 = Timer::get_tick();

  if(keyIdx == 0)
    ::printf("SolveKeyGPU Thread GPU#%d: 2^%.2f kangaroos [%.1fs]\n",ph->gpuId,log2((double)ph->nbKangaroo),(t1-t0));

  ph->hasStarted = true;

  while(!endOfSearch) {

    gpu->Launch(gpuFound);
    counters[thId] += ph->nbKangaroo * NB_RUN;

    if( clientMode ) {

      for(int i=0;i<(int)gpuFound.size();i++)
        dps.push_back(gpuFound[i]);

      double now = Timer::get_tick();
      if(now - lastSent > SEND_PERIOD) {
        LOCK(ghMutex);
        SendToServer(dps,ph->threadId,ph->gpuId,&deadWalkers);
        for(size_t i = 0; i < deadWalkers.size(); i++) {
          uint32_t kIdx = deadWalkers[i];
          if(kIdx >= ph->nbKangaroo)
            continue;
          Int px;
          Int py;
          Int d;
          CreateHerd(1,&px,&py,&d,kIdx % 2,false);
          gpu->SetKangaroo(kIdx,&px,&py,&d);
        }
        UNLOCK(ghMutex);
        lastSent = now;
      }

    } else {

      if(gpuFound.size() > 0) {

        LOCK(ghMutex);

        for(int g = 0; !endOfSearch && g < gpuFound.size(); g++) {

          uint32_t kType = (uint32_t)(gpuFound[g].kIdx % 2);

          if(!AddToTable(&gpuFound[g].x,&gpuFound[g].d,kType)) {
            // Collision inside the same herd
            // We need to reset the kangaroo
            Int px;
            Int py;
            Int d;
            CreateHerd(1,&px,&py,&d,kType,false);
            gpu->SetKangaroo(gpuFound[g].kIdx,&px,&py,&d);
            collisionInSameHerd++;
          }

        }
        UNLOCK(ghMutex);
      }

    }

    // Save request
    if(saveRequest && !endOfSearch) {
      // Get kangaroos
      if(saveKangaroo)
        gpu->GetKangaroos(ph->px,ph->py,ph->distance);
      ph->isWaiting = true;
      LOCK(saveMutex);
      ph->isWaiting = false;
      UNLOCK(saveMutex);
    }

  }


  safe_delete_array(ph->px);
  safe_delete_array(ph->py);
  safe_delete_array(ph->distance);
  delete gpu;

#else

  ph->hasStarted = true;

#endif

  ph->isRunning = false;

}

// ----------------------------------------------------------------------------

#ifdef WIN64
DWORD WINAPI _SolveKeyCPU(LPVOID lpParam) {
#else
void *_SolveKeyCPU(void *lpParam) {
#endif
  TH_PARAM *p = (TH_PARAM *)lpParam;
  p->obj->SolveKeyCPU(p);
  return 0;
}

#ifdef WIN64
DWORD WINAPI _SolveKeyGPU(LPVOID lpParam) {
#else
void *_SolveKeyGPU(void *lpParam) {
#endif
  TH_PARAM *p = (TH_PARAM *)lpParam;
  p->obj->SolveKeyGPU(p);
  return 0;
}

// ----------------------------------------------------------------------------

void Kangaroo::CreateHerd(int nbKangaroo,Int *px,Int *py,Int *d,int firstType,bool lock) {

  vector<Int> pk;
  vector<Point> S;
  vector<Point> Sp;
  pk.reserve(nbKangaroo);
  S.reserve(nbKangaroo);
  Sp.reserve(nbKangaroo);
  Point Z;
  Z.Clear();

  // Choose random starting distance
  if(lock) LOCK(ghMutex);

  for(uint64_t j = 0; j<nbKangaroo; j++) {

#ifdef USE_SYMMETRY

    // Tame in [0..N/2]
    d[j].Rand(rangePower - 1);
    if((j+ firstType) % 2 == WILD) {
      // Wild in [-N/4..N/4]
      d[j].ModSubK1order(&rangeWidthDiv4);
    }

#else

    // Tame in [0..N]
    d[j].Rand(rangePower);
    if((j + firstType) % 2 == WILD) {
      // Wild in [-N/2..N/2]
      d[j].ModSubK1order(&rangeWidthDiv2);
    }

#endif

    pk.push_back(d[j]);

  }

  if(lock) UNLOCK(ghMutex);

  // Compute starting pos
  S = secp->ComputePublicKeys(pk);

  for(uint64_t j = 0; j<nbKangaroo; j++) {
    if((j + firstType) % 2 == TAME) {
      Sp.push_back(Z);
    } else {
      Sp.push_back(keyToSearch);
    }
  }

  S = secp->AddDirect(Sp,S);

  for(uint64_t j = 0; j<nbKangaroo; j++) {

    px[j].Set(&S[j].x);
    py[j].Set(&S[j].y);

#ifdef USE_SYMMETRY
    // Equivalence symmetry class switch
    if( py[j].ModPositiveK1() )
      d[j].ModNegK1order();
#endif

  }

}

// ----------------------------------------------------------------------------

void Kangaroo::CreateJumpTable() {

#ifdef USE_SYMMETRY
  int jumpBit = rangePower / 2;
#else
  int jumpBit = rangePower / 2 + 1;
#endif

  if(jumpBit > 128) jumpBit = 128;
  int maxRetry = 100;
  bool ok = false;
  double distAvg;
  double maxAvg = pow(2.0,(double)jumpBit - 0.95);
  double minAvg = pow(2.0,(double)jumpBit - 1.05);
  //::printf("Jump Avg distance min: 2^%.2f\n",log2(minAvg));
  //::printf("Jump Avg distance max: 2^%.2f\n",log2(maxAvg));
  
  // Kangaroo jumps
  // Constant seed for compatibilty of workfiles
  rseed(0x600DCAFE);

#ifdef USE_SYMMETRY
  Int old;
  old.Set(Int::GetFieldCharacteristic());
  Int u;
  Int v;
  u.SetInt32(1);
  u.ShiftL(jumpBit/2);
  u.AddOne();
  while(!u.IsProbablePrime()) {
    u.AddOne();
    u.AddOne();
  }
  v.Set(&u);
  v.AddOne();
  v.AddOne();
  while(!v.IsProbablePrime()) {
    v.AddOne();
    v.AddOne();
  }
  Int::SetupField(&old);

  ::printf("U= %s\n",u.GetBase16().c_str());
  ::printf("V= %s\n",v.GetBase16().c_str());
#endif

  // Positive only
  // When using symmetry, the sign is switched by the symmetry class switch
  while(!ok && maxRetry>0 ) {
    Int totalDist;
    totalDist.SetInt32(0);
#ifdef USE_SYMMETRY
    for(int i = 0; i < NB_JUMP/2; ++i) {
      jumpDistance[i].Rand(jumpBit/2);
      jumpDistance[i].Mult(&u);
      if(jumpDistance[i].IsZero())
        jumpDistance[i].SetInt32(1);
      totalDist.Add(&jumpDistance[i]);
    }
    for(int i = NB_JUMP / 2; i < NB_JUMP; ++i) {
      jumpDistance[i].Rand(jumpBit/2);
      jumpDistance[i].Mult(&v);
      if(jumpDistance[i].IsZero())
        jumpDistance[i].SetInt32(1);
      totalDist.Add(&jumpDistance[i]);
    }
#else
    for(int i = 0; i < NB_JUMP; ++i) {
      jumpDistance[i].Rand(jumpBit);
      if(jumpDistance[i].IsZero())
        jumpDistance[i].SetInt32(1);
      totalDist.Add(&jumpDistance[i]);
  }
#endif
    distAvg = totalDist.ToDouble() / (double)(NB_JUMP);
    ok = distAvg>minAvg && distAvg<maxAvg;
    maxRetry--;
  }

  for(int i = 0; i < NB_JUMP; ++i) {
    Point J = secp->ComputePublicKey(&jumpDistance[i]);
    jumpPointx[i].Set(&J.x);
    jumpPointy[i].Set(&J.y);
  }

  ::printf("Jump Avg distance: 2^%.2f\n",log2(distAvg));

  unsigned long seed = Timer::getSeed32();
  rseed(seed);

}

// ----------------------------------------------------------------------------

void Kangaroo::ComputeExpected(double dp,double *op,double *ram,double *overHead) {

  // Compute expected number of operation and memory

#ifdef USE_SYMMETRY
  double gainS = 1.0 / sqrt(2.0);
#else
  double gainS = 1.0;
#endif

  // Kangaroo number
  double k = (double)totalRW;

  // Range size
  double N = pow(2.0,(double)rangePower);

  // theta
  double theta = pow(2.0,dp);

  // Z0
  double Z0 = (2.0 * (2.0 - sqrt(2.0)) * gainS) * sqrt(M_PI);

  // Average for DP = 0
  double avgDP0 = Z0 * sqrt(N);

  // DP Overhead
  *op = Z0 * pow(N * (k * theta + sqrt(N)),1.0 / 3.0);

  *ram = (double)sizeof(HASH_ENTRY) * (double)HASH_SIZE + // Table
         (double)sizeof(ENTRY *) * (double)(HASH_SIZE * 4) + // Allocation overhead
         (double)(sizeof(ENTRY) + sizeof(ENTRY *)) * (*op / theta); // Entries

  *ram /= (1024.0*1024.0);

  if(overHead)
    *overHead = *op/avgDP0;

}

// ----------------------------------------------------------------------------

void Kangaroo::InitRange() {

  rangeWidth.Set(&rangeEnd);
  rangeWidth.Sub(&rangeStart);
  rangePower = rangeWidth.GetBitLength();
  ::printf("Range width: 2^%d\n",rangePower);
  rangeWidthDiv2.Set(&rangeWidth);
  rangeWidthDiv2.ShiftR(1);
  rangeWidthDiv4.Set(&rangeWidthDiv2);
  rangeWidthDiv4.ShiftR(1);
  rangeWidthDiv8.Set(&rangeWidthDiv4);
  rangeWidthDiv8.ShiftR(1);

}

void Kangaroo::InitSearchKey() {

  Int SP;
  SP.Set(&rangeStart);
#ifdef USE_SYMMETRY
  SP.ModAddK1order(&rangeWidthDiv2);
#endif
  if(!SP.IsZero()) {
    Point RS = secp->ComputePublicKey(&SP);
    RS.y.ModNeg();
    keyToSearch = secp->AddDirect(keysToSearch[keyIdx],RS);
  } else {
    keyToSearch = keysToSearch[keyIdx];
  }
  keyToSearchNeg = keyToSearch;
  keyToSearchNeg.y.ModNeg();

}

// ----------------------------------------------------------------------------

RunResult Kangaroo::Run(int nbThread,std::vector<int> gpuId,std::vector<int> gridSize) {

  double t0 = Timer::get_tick();
  g_stopRequested = 0;
  SetRunResult(RESULT_NO_KEY);

  if(signal(SIGINT,solver_sig_handler) == SIG_ERR)
    ::printf("\nWarning: can't install SIGINT handler\n");
  if(signal(SIGTERM,solver_sig_handler) == SIG_ERR)
    ::printf("\nWarning: can't install SIGTERM handler\n");

  nbCPUThread = nbThread;
  nbGPUThread = (useGpu ? (int)gpuId.size() : 0);
  totalRW = 0;

#ifndef WITHGPU

  if(nbGPUThread>0) {
    ::printf("GPU code not compiled, use -DWITHGPU when compiling.\n");
    nbGPUThread = 0;
  }

#endif

  uint64_t totalThread = (uint64_t)nbCPUThread + (uint64_t)nbGPUThread;
  if(totalThread == 0) {
    ::printf("No CPU or GPU thread, exiting.\n");
    SetRunResult(RESULT_RUNTIME_ERROR);
    return GetRunResult();
  }

  TH_PARAM *params = (TH_PARAM *)malloc(totalThread * sizeof(TH_PARAM));
  THREAD_HANDLE *thHandles = (THREAD_HANDLE *)malloc(totalThread * sizeof(THREAD_HANDLE));

  memset(params, 0,totalThread * sizeof(TH_PARAM));
  memset(counters, 0, sizeof(counters));
  ::printf("Number of CPU thread: %d\n", nbCPUThread);

#ifdef WITHGPU

  // Compute grid size
  for(int i = 0; i < nbGPUThread; i++) {
    int x = gridSize[2ULL * i];
    int y = gridSize[2ULL * i + 1ULL];
    if(!GPUEngine::GetGridSize(gpuId[i],&x,&y)) {
      SetRunResult(RESULT_RUNTIME_ERROR);
      return GetRunResult();
    } else {
      params[nbCPUThread + i].gridSizeX = x;
      params[nbCPUThread + i].gridSizeY = y;
    }
    params[nbCPUThread + i].nbKangaroo = (uint64_t)GPU_GRP_SIZE * x * y;
    totalRW += params[nbCPUThread + i].nbKangaroo;
  }

#endif

  totalRW += nbCPUThread * (uint64_t)CPU_GRP_SIZE;

  // Set starting parameters
  if( clientMode ) {
    // Retrieve config from server
    if( !GetConfigFromServer() ) {
      SetRunResult(RESULT_RUNTIME_ERROR);
      return GetRunResult();
    }
    // Client save only kangaroos, force -ws
    if(workFile.length()>0)
      saveKangaroo = true;
  }

  InitRange();
  CreateJumpTable();

  ::printf("Number of kangaroos: 2^%.2f\n",log2((double)totalRW));

  if( !clientMode ) {

    // Compute suggested distinguished bits number for less than 5% overhead (see README)
    double dpOverHead;
    int suggestedDP = (int)((double)rangePower / 2.0 - log2((double)totalRW));
    if(suggestedDP<0) suggestedDP=0;
    ComputeExpected((double)suggestedDP,&expectedNbOp,&expectedMem,&dpOverHead);
    while(dpOverHead>1.05 && suggestedDP>0) {
      suggestedDP--;
      ComputeExpected((double)suggestedDP,&expectedNbOp,&expectedMem,&dpOverHead);
    }

    if(initDPSize < 0)
      initDPSize = suggestedDP;

    ComputeExpected((double)initDPSize,&expectedNbOp,&expectedMem);
    if(nbLoadedWalk == 0) ::printf("Suggested DP: %d\n",suggestedDP);
    ::printf("Expected operations: 2^%.2f\n",log2(expectedNbOp));
    ::printf("Expected RAM: %.1fMB\n",expectedMem);

  } else {

    keyIdx = 0;
    InitSearchKey();

  }

  SetDP(initDPSize);

  // Fetch kangaroos (if any)
  if(!FectchKangaroos(params)) {
    SetRunResult(RESULT_RUNTIME_ERROR);
    return GetRunResult();
  }

//#define STATS
#ifdef STATS

    CPU_GRP_SIZE = 1024;
    for(; CPU_GRP_SIZE <= 1024; CPU_GRP_SIZE *= 4) {

      uint64_t totalCount = 0;
      uint64_t totalDead = 0;

#endif

    for(keyIdx = 0; keyIdx < keysToSearch.size(); keyIdx++) {

      InitSearchKey();

      endOfSearch = false;
      collisionInSameHerd = 0;

      // Reset conters
      memset(counters,0,sizeof(counters));

      // Lanch CPU threads
      for(int i = 0; i < nbCPUThread; i++) {
        params[i].threadId = i;
        params[i].isRunning = true;
        thHandles[i] = LaunchThread(_SolveKeyCPU,params + i);
      }

#ifdef WITHGPU

      // Launch GPU threads
      for(int i = 0; i < nbGPUThread; i++) {
        int id = nbCPUThread + i;
        params[id].threadId = 0x80L + i;
        params[id].isRunning = true;
        params[id].gpuId = gpuId[i];
        thHandles[id] = LaunchThread(_SolveKeyGPU,params + id);
      }

#endif


      // Wait for end
      Process(params,"MK/s");
      JoinThreads(thHandles,nbCPUThread + nbGPUThread);
      FreeHandles(thHandles,nbCPUThread + nbGPUThread);
      hashTable.Reset();

#ifdef STATS

      uint64_t count = getCPUCount() + getGPUCount();
      totalCount += count;
      totalDead += collisionInSameHerd;
      double SN = pow(2.0,rangePower / 2.0);
      double avg = (double)totalCount / (double)(keyIdx + 1);
      ::printf("\n[%3d] 2^%.3f Dead:%d Avg:2^%.3f DeadAvg:%.1f (%.3f %.3f sqrt(N))\n",
                              keyIdx, log2((double)count), (int)collisionInSameHerd, 
                              log2(avg), (double)totalDead / (double)(keyIdx + 1),
                              avg/SN,expectedNbOp/SN);
    }
    string fName = "DP" + ::to_string(dpSize) + ".txt";
    FILE *f = fopen(fName.c_str(),"a");
    fprintf(f,"%d %f\n",CPU_GRP_SIZE*nbCPUThread,(double)totalCount);
    fclose(f);

#endif

  }

  double t1 = Timer::get_tick();

  ::printf("\nDone: Total time %s \n" , GetTimeStr(t1-t0+offsetTime).c_str());
  return GetRunResult();

}


