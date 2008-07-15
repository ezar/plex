/*
* SAP-Announcement Support for XBMC
* Copyright (c) 2008 elupus (Joakim Plate)
*
* This program is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation; either version 2 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program; if not, write to the Free Software
* Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
*/

#include "stdafx.h"
#include "SAPDirectory.h"
#include "Util.h"
#include "FileItem.h"
#include <Ws2tcpip.h>

CSAPSessions g_sapsessions;

namespace SDP
{
  /* SAP is always on that port */
  #define SAP_PORT 9875
  /* Global-scope SAP address */
  #define SAP_V4_GLOBAL_ADDRESS   "224.2.127.254"
  /* Organization-local SAP address */
  #define SAP_V4_ORG_ADDRESS      "239.195.255.255"
  /* Local (smallest non-link-local scope) SAP address */
  #define SAP_V4_LOCAL_ADDRESS    "239.255.255.255"
  /* Link-local SAP address */
  #define SAP_V4_LINK_ADDRESS     "224.0.0.255"
  #define ADD_SESSION 1

  #define SAP_V6_1 "FF0"
  /* Scope is inserted between them */
  #define SAP_V6_2 "::2:7FFE"

  int parse_sap(const char* data, int len, struct sap_desc *h)
  {
    const char* data_orig = data;

    if(len < 4)
      return -1;

    memset(h, 0, sizeof(sap_desc));

    h->version    = (data[0] >> 5) & 0x7;
    h->addrtype   = (data[0] >> 4) & 0x1;
    h->msgtype    = (data[0] >> 2) & 0x1;
    h->encrypted  = (data[0] >> 1) & 0x1;
    h->compressed = (data[0] >> 0) & 0x1;
    
    h->authlen    =  data[1];
    h->msgid      = ((data[2] << 8) | data[3]);

    data += 4;
    len  -= 4;

    if(h->addrtype) {

      if(len < 16) {
        CLog::Log(LOGERROR, "%s - too little data for origin address", __FUNCTION__);
        return -1;
      }

      CLog::Log(LOGERROR, "%s - ipv6 addresses currently unsupported", __FUNCTION__);
      return -1;
    } else {

      if(len < 4) {
        CLog::Log(LOGERROR, "%s - too little data for origin address", __FUNCTION__);
        return -1;
      }

      h->origin = inet_ntoa(*(struct in_addr*)data);
      data += 4;
      len  -= 4;
    }

    /* skip authentication data */
    data += h->authlen;
    len  -= h->authlen;

    /* payload type may be missing, then it's assumed to be sdp */
    if(data[0] == 'v' 
    && data[1] == '='
    && data[2] == '0') {
      h->payload_type = "application/sdp";
      return data - data_orig;
    }

    /* read payload type */
    int payload_size = strnlen(data, len);
    if (payload_size == len)
      return -1;
    h->payload_type.assign(data, payload_size);

    data += payload_size+1;
    len  -= payload_size+1;

    return data - data_orig;
  }

  static int parse_sdp_line(const char* data, std::string& type, std::string& value)
  {
    const char* data2 = data;
    int l;

    l = strcspn(data, "=\n\r");
    type.assign(data, l);
    data += l;
    if(*data == '=')
      data++;

    l = strcspn(data, "\n\r");
    value.assign(data, l);
    data += l;
    data += strspn(data, "\n\r");

    return data - data2;
  }

  static int parse_sdp_type(const char** data, std::string type, std::string& value)
  {
    std::string type2;
    const char* data2 = *data;

    while(*data2 != 0) {
      data2 += parse_sdp_line(data2, type2, value);
      if(type2 == type) {
        *data = data2;
        return 1;
      }
    }
    return 0;
  }
  
  int parse_sdp_token(const char* data, std::string &value)
  {
    int l;
    l = strcspn(data, " \n\r");
    value.assign(data, l);
    if(data[l] == '\0')
      return l;
    else
      return l+1;
  }

  int parse_sdp_token(const char* data, int &value)
  {
    int l;
    std::string str;
    l = parse_sdp_token(data, str);
    value = atoi(str.c_str());
    return l;
  }

  int parse_sdp_origin(const char* data, struct sdp_desc_origin *o)
  {
    const char *data2 = data;
    data += parse_sdp_token(data, o->username);
    data += parse_sdp_token(data, o->sessionid);
    data += parse_sdp_token(data, o->sessionver);
    data += parse_sdp_token(data, o->nettype);
    data += parse_sdp_token(data, o->addrtype);
    data += parse_sdp_token(data, o->address);
    return data - data2;
  }

  int parse_sdp(const char* data, struct sdp_desc* sdp)
  {
    const char *data2 = data;
    std::string value;

    // SESSION DESCRIPTION  
    if(parse_sdp_type(&data, "v", value)) {
      sdp->version = value;
    } else
      return 0;

    if(parse_sdp_type(&data, "o", value)) {
      sdp->origin = value;
    } else
      return 0;

    if(parse_sdp_type(&data, "s", value)) {
      sdp->name = value;
    } else
      return 0;

    if(parse_sdp_type(&data, "i", value))
      sdp->title = value;

    if(parse_sdp_type(&data, "b", value))
      sdp->bandwidth = value;

    while(true) {
      if(parse_sdp_type(&data, "a", value))
        sdp->attributes.push_back(value);
      else
        break;
    }

    // TIME DESCRIPTIONS
    while(true) {
      sdp_desc_time time;
      if(parse_sdp_type(&data, "t", value))
        time.active = value;
      else
        break;

      if(parse_sdp_type(&data, "r", value))
        time.repeat = value;

      sdp->times.push_back(time);
    }

    // MEDIA DESCRIPTIONS
    while(true) {
      sdp_desc_media media;
      if(parse_sdp_type(&data, "m", value))
        media.name = value;
      else
        break;

      if(parse_sdp_type(&data, "i", value))
        media.title = value;

      if(parse_sdp_type(&data, "c", value))
        media.connection = value;

      while(true) {
        if(parse_sdp_type(&data, "a", value))
          media.attributes.push_back(value);
        else
          break;
      }

      sdp->media.push_back(media);
    }

    return data - data2;
  }
}


using namespace SDP;


CSAPSessions::CSAPSessions()
{
}

CSAPSessions::~CSAPSessions()
{
  StopThread();
}

bool CSAPSessions::ParseAnnounce(char* data, int len)
{
  CSingleLock lock(m_section);

  struct sap_desc header;
  int size = parse_sap(data, len, &header);
  if(size < 0)
  {
    CLog::Log(LOGERROR, "%s - failed to parse sap announcment", __FUNCTION__);
    return false;
  }

  // we only want sdp payloads
  if(header.payload_type != "application/sdp") 
  {
    CLog::Log(LOGERROR, "%s - unknown payload type '%s'", __FUNCTION__, header.payload_type.c_str());
    return false;
  }

  data += size;
  len  -= size;

  sdp_desc desc;
  if(parse_sdp(data, &desc) < 0)
  {
    CLog::Log(LOGERROR, "%s - failed to parse sdp [ --->\n%s\n<--- ]", __FUNCTION__, data);
    return false;
  }

  // check if we can find this session in our cache
  for(std::vector<CSession>::iterator it = m_sessions.begin(); it != m_sessions.end(); it++)
  {
    if(it->origin         == header.origin
    && it->msgid          == header.msgid
    && it->payload_origin == desc.origin)
    {
      if(header.msgtype == 1)
      {
        m_sessions.erase(it);
        return true;
      }

      // should be improved in the case of sdp
      it->timeout = GetTickCount() + 60*60*1000;
      return true;
    }
  }

  if(header.msgtype == 1)
    return true;

  sdp_desc_origin origin;
  if(parse_sdp_origin(desc.origin.c_str(), &origin) < 0)
  {
    CLog::Log(LOGERROR, "%s - failed to parse origin '%'", __FUNCTION__, desc.origin.c_str());
    return false;
  }

  // add a new session to our buffer
  CStdString path, user;
  user = origin.username;
  CUtil::URLEncode(user);
  path.Format("sap://%s@%s/%s/%s/0x%x.sdp", user, origin.address, origin.nettype, origin.addrtype, origin.sessionid);
  CSession session;
  session.path           = path;
  session.origin         = header.origin;
  session.msgid          = header.msgid;
  session.payload_type   = header.payload_type;
  session.payload_origin = desc.origin;
  session.payload.assign(data, len);
  session.timeout = GetTickCount() + 60*60*1000;
  m_sessions.push_back(session);
  return true;
}

void CSAPSessions::Process()
{
  struct ip_mreq mreq;
  SOCKET sock;

  sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if(sock == INVALID_SOCKET)
    return;

  unsigned long nonblocking = 1;
  ioctlsocket(sock, FIONBIO, &nonblocking);

  const char one = 1;
  setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

  /* bind to SAP port */
  struct sockaddr_in addr;
  addr.sin_family           = AF_INET;
  addr.sin_addr.S_un.S_addr = INADDR_ANY;
  addr.sin_port             = htons(SAP_PORT);
  if(bind(sock, (const sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
    closesocket(sock);
    return;
  }

  /* subscribe to all SAP multicast addresses */
  mreq.imr_multiaddr.S_un.S_addr = inet_addr(SAP_V4_GLOBAL_ADDRESS);
  mreq.imr_interface.S_un.S_addr = INADDR_ANY;
  setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, (char*)&mreq, sizeof(mreq));

  mreq.imr_multiaddr.S_un.S_addr = inet_addr(SAP_V4_ORG_ADDRESS);
  mreq.imr_interface.S_un.S_addr = INADDR_ANY;
  setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, (char*)&mreq, sizeof(mreq));

  mreq.imr_multiaddr.S_un.S_addr = inet_addr(SAP_V4_LOCAL_ADDRESS);
  mreq.imr_interface.S_un.S_addr = INADDR_ANY;
  setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, (char*)&mreq, sizeof(mreq));

  mreq.imr_multiaddr.S_un.S_addr = inet_addr(SAP_V4_LINK_ADDRESS);
  mreq.imr_interface.S_un.S_addr = INADDR_ANY;
  setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, (char*)&mreq, sizeof(mreq));

  /* start listening for announces */
  struct fd_set  readfds;
  struct timeval timeout;
  int count;

  char data[65507+1];

  while(!m_bStop)
  {
    timeout.tv_sec  = 5;
    timeout.tv_usec = 0;

    FD_ZERO(&readfds);
    FD_SET(sock, &readfds);

    count = select(0, &readfds, NULL, NULL, &timeout);
    if(count == SOCKET_ERROR) {
      closesocket(sock);
      return;
    }


    if(FD_ISSET(sock, &readfds)) {
      count = recv(sock, data, sizeof(data), 0);
  
      if(count == SOCKET_ERROR) {
        closesocket(sock);
        return;
      }
      /* data must be string terminated for parsers */
      data[count] = '\0';
      ParseAnnounce(data, count);
    }

  }

  closesocket(sock);
}

namespace DIRECTORY
{

  CSAPDirectory::CSAPDirectory(void)
  {
  }



  CSAPDirectory::~CSAPDirectory(void)
  {  
  }

  bool CSAPDirectory::GetDirectory(const CStdString& strPath, CFileItemList &items)
  {
    if(strPath != "sap://")
      return false;

    CSingleLock lock(g_sapsessions.m_section);

    if(g_sapsessions.ThreadHandle() == NULL)
      g_sapsessions.Create();

    // check if we can find this session in our cache
    for(std::vector<CSAPSessions::CSession>::iterator it = g_sapsessions.m_sessions.begin(); it != g_sapsessions.m_sessions.end(); it++)
    {

      if(it->payload_type != "application/sdp")
      {
        CLog::Log(LOGDEBUG, "%s - unknown sdp payload type [%s]", __FUNCTION__, it->payload_type);
        continue;
      }
      struct sdp_desc desc;
      if(parse_sdp(it->payload.c_str(), &desc) <= 0)
      {
        CLog::Log(LOGDEBUG, "%s - invalid sdp payload [ --->\n%s\n<--- ]", __FUNCTION__, it->payload);
        continue;
      }

      struct sdp_desc_origin origin;
      if(parse_sdp_origin(desc.origin.c_str(), &origin) <= 0)
      {
        CLog::Log(LOGDEBUG, "%s - invalid sdp origin [ --->\n%s\n<--- ]", __FUNCTION__, desc.origin.c_str());
        continue;
      }

      CFileItemPtr item(new CFileItem());

      if(desc.title.length() > 0)
        item->SetLabel(desc.title);
      else
        item->SetLabel(desc.name);

      item->m_strPath = it->path;
      items.Add(item);
    }

    return true;
  }


}
