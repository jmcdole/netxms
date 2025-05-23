/* 
** NetXMS - Network Management System
** Database Abstraction Library
** Copyright (C) 2008-2025 Raden Solutions
**
** This program is free software; you can redistribute it and/or modify
** it under the terms of the GNU Lesser General Public License as published by
** the Free Software Foundation; either version 3 of the License, or
** (at your option) any later version.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU General Public License for more details.
**
** You should have received a copy of the GNU Lesser General Public License
** along with this program; if not, write to the Free Software
** Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
**
** File: dbcp.cpp
**
**/

#include "libnxdb.h"

static bool s_initialized = false;
static DB_DRIVER m_driver;
static TCHAR m_server[256];
static TCHAR m_login[256];
static TCHAR m_password[256];
static TCHAR m_dbName[256];
static TCHAR m_schema[256];

static int m_basePoolSize;
static int m_maxPoolSize;
static int m_cooldownTime;
static int m_connectionTTL;

static Mutex m_poolAccessMutex;
static ObjectArray<PoolConnectionInfo> m_connections;
static THREAD m_maintThread = INVALID_THREAD_HANDLE;
static Condition m_condShutdown(true);
static Condition m_condRelease(false);

#define DEBUG_TAG _T("db.cpool")

/**
 * Create connections on pool initialization
 */
static bool DBConnectionPoolPopulate()
{
	TCHAR errorText[DBDRV_MAX_ERROR_TEXT];
	bool success = false;

	m_poolAccessMutex.lock();
	for(int i = 0; i < m_basePoolSize; i++)
	{
      PoolConnectionInfo *conn = new PoolConnectionInfo;
      conn->handle = DBConnect(m_driver, m_server, m_dbName, m_login, m_password, m_schema, errorText);
      if (conn->handle != NULL)
      {
         conn->inUse = false;
         conn->resetOnRelease = false;
         conn->connectTime = time(NULL);
         conn->lastAccessTime = conn->connectTime;
         conn->usageCount = 0;
         conn->srcFile[0] = 0;
         conn->srcLine = 0;
         m_connections.add(conn);
         nxlog_debug_tag(DEBUG_TAG, 3, _T("Connection %p created"), conn);
         success = true;
      }
      else
      {
         nxlog_debug_tag(DEBUG_TAG, 3, _T("Cannot create DB connection %d (%s)"), i, errorText);
         delete conn;
      }
	}
	m_poolAccessMutex.unlock();
	return success;
}

/**
 * Shrink connection pool up to base size when possible
 */
static void DBConnectionPoolShrink()
{
	m_poolAccessMutex.lock();

   time_t now = time(nullptr);
   for(int i = m_basePoolSize; i < m_connections.size(); i++)
	{
      PoolConnectionInfo *conn = m_connections.get(i);
		if (!conn->inUse && (now - conn->lastAccessTime > m_cooldownTime))
		{
			DBDisconnect(conn->handle);
	      nxlog_debug_tag(DEBUG_TAG, 3, _T("Connection %p terminated"), conn);
         m_connections.remove(i);
         i--;
		}
	}

	m_poolAccessMutex.unlock();
}

/*
 * Reset connection
 */
static bool ResetConnection(PoolConnectionInfo *conn)
{
	time_t now = time(NULL);
	DBDisconnect(conn->handle);

	TCHAR errorText[DBDRV_MAX_ERROR_TEXT];
	conn->handle = DBConnect(m_driver, m_server, m_dbName, m_login, m_password, m_schema, errorText);
	if (conn->handle != NULL)
   {
		conn->connectTime = now;
		conn->lastAccessTime = now;
		conn->usageCount = 0;

		nxlog_debug_tag(DEBUG_TAG, 3, _T("Connection %p reconnected"), conn);
	}
   else
   {
		nxlog_debug_tag(DEBUG_TAG, 3, _T("Connection %p reconnect failure (%s)"), conn, errorText);
	}
   conn->resetOnRelease = false;
   return conn->handle != nullptr;
}

/**
 * Callback for sorting reset list
 */
static int ResetListSortCallback(const PoolConnectionInfo **e1, const PoolConnectionInfo **e2)
{
   return (*e1)->usageCount > (*e2)->usageCount ? -1 : ((*e1)->usageCount == (*e2)->usageCount ? 0 : 1);
}

/**
 * Reset expired connections
 */
static void ResetExpiredConnections()
{
   time_t now = time(nullptr);

   m_poolAccessMutex.lock();

	int i, availCount = 0;
   ObjectArray<PoolConnectionInfo> reconnList(m_connections.size(), 16, Ownership::False);
	for(i = 0; i < m_connections.size(); i++)
	{
		PoolConnectionInfo *conn = m_connections.get(i);
		if (!conn->inUse)
      {
         availCount++;
         if (now - conn->connectTime > m_connectionTTL)
         {
            reconnList.add(conn);
         }
      }
	}
	
   int count = std::min(availCount / 2 + 1, reconnList.size()); // reset no more than 50% of available connections
   if (count < reconnList.size())
   {
      reconnList.sort(ResetListSortCallback);
      while(reconnList.size() > count)
         reconnList.remove(count);
   }

   for(i = 0; i < count; i++)
      reconnList.get(i)->inUse = true;
   m_poolAccessMutex.unlock();

   // do reconnects
   for(i = 0; i < count; i++)
	{
   	PoolConnectionInfo *conn = reconnList.get(i);
   	bool success = ResetConnection(conn);
   	m_poolAccessMutex.lock();
		if (success)
		{
			conn->inUse = false;
		}
		else
		{
			m_connections.remove(conn);
		}
		m_poolAccessMutex.unlock();
	}
}

/**
 * Pool maintenance thread
 */
static THREAD_RESULT THREAD_CALL MaintenanceThread(void *arg)
{
   ThreadSetName("DBPoolMaint");
   nxlog_debug_tag(DEBUG_TAG, 1, _T("Database Connection Pool maintenance thread started"));

   while(!m_condShutdown.wait((m_connectionTTL > 0) ? m_connectionTTL * 750 : 300000))
   {
      DBConnectionPoolShrink();
      if (m_connectionTTL > 0)
      {
         ResetExpiredConnections();
      }
   }

	nxlog_debug_tag(DEBUG_TAG, 1, _T("Database Connection Pool maintenance thread stopped"));
   return THREAD_OK;
}

/**
 * Start connection pool
 */
bool LIBNXDB_EXPORTABLE DBConnectionPoolStartup(DB_DRIVER driver, const TCHAR *server, const TCHAR *dbName,
																const TCHAR *login, const TCHAR *password, const TCHAR *schema,
																int basePoolSize, int maxPoolSize, int cooldownTime,
																int connTTL)
{
   if (s_initialized)
      return true;   // already initialized

	m_driver = driver;
	_tcslcpy(m_server, CHECK_NULL_EX(server), 256);
	_tcslcpy(m_dbName, CHECK_NULL_EX(dbName), 256);
	_tcslcpy(m_login, CHECK_NULL_EX(login), 256);
	_tcslcpy(m_password, CHECK_NULL_EX(password), 256);
	_tcslcpy(m_schema, CHECK_NULL_EX(schema), 256);

	m_basePoolSize = basePoolSize;
	m_maxPoolSize = maxPoolSize;
	m_cooldownTime = cooldownTime;
   m_connectionTTL = connTTL;

   m_connections.setOwner(Ownership::True);

	if (!DBConnectionPoolPopulate())
	{
	   // cannot open at least one connection
	   return false;
	}

   m_maintThread = ThreadCreateEx(MaintenanceThread, 0, NULL);

   s_initialized = true;
	nxlog_debug_tag(DEBUG_TAG, 1, _T("Database Connection Pool initialized"));

	return true;
}

/**
 * Shutdown connection pool
 */
void LIBNXDB_EXPORTABLE DBConnectionPoolShutdown()
{
   if (!s_initialized)
      return;

   m_condShutdown.set();
   ThreadJoin(m_maintThread);

   for(int i = 0; i < m_connections.size(); i++)
	{
      DBDisconnect(m_connections.get(i)->handle);
	}

   m_connections.clear();

   s_initialized = false;
	nxlog_debug_tag(DEBUG_TAG, 1, _T("Database Connection Pool terminated"));
}

/**
 * Reset connection pool (reconnect all idle connections and mark non-idle for reconnect after release)
 */
void LIBNXDB_EXPORTABLE DBConnectionPoolReset()
{
   m_poolAccessMutex.lock();

   for(int i = 0; i < m_connections.size(); i++)
   {
      PoolConnectionInfo *conn = m_connections.get(i);
      if (conn->inUse)
      {
         conn->resetOnRelease = true;
      }
      else if (m_connections.size() > m_basePoolSize)
      {
         DBDisconnect(conn->handle);
         m_connections.remove(i);
         i--;
      }
      else
      {
         if (!ResetConnection(conn))
         {
            m_connections.remove(i);
            i--;
         }
      }
   }

   m_poolAccessMutex.unlock();
}

/**
 * Acquire connection from pool. This function never fails - if it's impossible to acquire
 * pooled connection, calling thread will be suspended until there will be connection available.
 */
DB_HANDLE LIBNXDB_EXPORTABLE __DBConnectionPoolAcquireConnection(const char *srcFile, int srcLine)
{
retry:
	m_poolAccessMutex.lock();

	DB_HANDLE handle = nullptr;

	// find less used connection
   uint32_t count = 0xFFFFFFFF;
	int index = -1;
   for(int i = 0; (i < m_connections.size()) && (count > 0); i++)
	{
      PoolConnectionInfo *conn = m_connections.get(i);
      if (!conn->inUse && (conn->usageCount < count))
      {
         count = conn->usageCount;
         index = i;
		}
	}

	if (index > -1)
	{
		PoolConnectionInfo *conn = m_connections.get(index);
		handle = conn->handle;
		conn->inUse = true;
		conn->lastAccessTime = time(nullptr);
		conn->usageCount++;
		strlcpy(conn->srcFile, srcFile, 128);
		conn->srcLine = srcLine;
	}
   else if (m_connections.size() < m_maxPoolSize)
	{
	   TCHAR errorText[DBDRV_MAX_ERROR_TEXT];
      PoolConnectionInfo *conn = new PoolConnectionInfo;
      conn->handle = DBConnect(m_driver, m_server, m_dbName, m_login, m_password, m_schema, errorText);
      if (conn->handle != nullptr)
      {
         conn->inUse = true;
         conn->resetOnRelease = false;
         conn->connectTime = time(nullptr);
         conn->lastAccessTime = conn->connectTime;
         conn->usageCount = 0;
         strlcpy(conn->srcFile, srcFile, 128);
         conn->srcLine = srcLine;
         m_connections.add(conn);
         handle = conn->handle;
         nxlog_debug_tag(DEBUG_TAG, 3, _T("Connection %p created"), conn);
      }
      else
      {
         nxlog_debug_tag(DEBUG_TAG, 3, _T("Cannot create additional DB connection (%s)"), errorText);
         delete conn;
      }
	}

	m_poolAccessMutex.unlock();

	if (handle == nullptr)
	{
   	nxlog_debug_tag(DEBUG_TAG, 1, _T("Database connection pool exhausted (call from %hs:%d)"), srcFile, srcLine);
      m_condRelease.wait(10000);
      nxlog_debug_tag(DEBUG_TAG, 5, _T("Retry acquire connection (call from %hs:%d)"), srcFile, srcLine);
      goto retry;
	}

   nxlog_debug_tag(DEBUG_TAG, 7, _T("Handle %p acquired (call from %hs:%d)"), handle, srcFile, srcLine);
	return handle;
}

/**
 * Release acquired connection
 */
void LIBNXDB_EXPORTABLE DBConnectionPoolReleaseConnection(DB_HANDLE handle)
{
	m_poolAccessMutex.lock();

   for(int i = 0; i < m_connections.size(); i++)
	{
      PoolConnectionInfo *conn = m_connections.get(i);
      if (conn->handle == handle)
		{
         conn->srcFile[0] = 0;
         conn->srcLine = 0;
         if (conn->resetOnRelease)
         {
            m_poolAccessMutex.unlock();
            bool success = ResetConnection(conn);
            m_poolAccessMutex.lock();
            if (success)
            {
               conn->inUse = false;
            }
            else
            {
               m_connections.remove(i);
            }
         }
         else
         {
            conn->inUse = false;
            conn->lastAccessTime = time(NULL);
         }
			break;
		}
	}

	m_poolAccessMutex.unlock();

   nxlog_debug_tag(DEBUG_TAG, 7, _T("Handle %p released"), handle);
   m_condRelease.pulse();
}

/**
 * Get current size of DB connection pool
 */
int LIBNXDB_EXPORTABLE DBConnectionPoolGetSize()
{
	m_poolAccessMutex.lock();
   int size = m_connections.size();
	m_poolAccessMutex.unlock();
   return size;
}

/**
 * Get number of acquired connections in DB connection pool
 */
int LIBNXDB_EXPORTABLE DBConnectionPoolGetAcquiredCount()
{
   int count = 0;
	m_poolAccessMutex.lock();
   for(int i = 0; i < m_connections.size(); i++)
      if (m_connections.get(i)->inUse)
         count++;
	m_poolAccessMutex.unlock();
   return count;
}

/**
 * Get copy of active DB connections.
 * Returned list must be deleted by the caller.
 */
ObjectArray<PoolConnectionInfo> LIBNXDB_EXPORTABLE *DBConnectionPoolGetConnectionList()
{
   ObjectArray<PoolConnectionInfo> *list = new ObjectArray<PoolConnectionInfo>(32, 32, Ownership::True);
   m_poolAccessMutex.lock();
   for(int i = 0; i < m_connections.size(); i++)
   {
      PoolConnectionInfo *curr = m_connections.get(i);
      if (curr->inUse)
      {
         PoolConnectionInfo *ci = new PoolConnectionInfo;
         memcpy(ci, curr, sizeof(PoolConnectionInfo));
         list->add(ci);
      }
   }
   m_poolAccessMutex.unlock();
   return list;
}
