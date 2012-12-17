﻿/*
** discovery.cpp
**
** Copyright © Quazaa Development Team, 2012.
** This file is part of QUAZAA (quazaa.sourceforge.net)
**
** Quazaa is free software; this file may be used under the terms of the GNU
** General Public License version 3.0 or later as published by the Free Software
** Foundation and appearing in the file LICENSE.GPL included in the
** packaging of this file.
**
** Quazaa is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
**
** Please review the following information to ensure the GNU General Public
** License version 3.0 requirements will be met:
** http://www.gnu.org/copyleft/gpl.html.
**
** You should have received a copy of the GNU General Public License version
** 3.0 along with Quazaa; if not, write to the Free Software Foundation,
** Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/

#include <QFile>
#include <QDateTime>

#include "discovery.h"
#include "discoveryservice.h"

#include "quazaasettings.h"

#ifdef _DEBUG
#include "debug_new.h"
#endif

Discovery::CDiscovery discoveryManager;

using namespace Discovery;


/**
 * @brief CDiscovery: Constructs a new discovery services manager.
 * @param parent
 */
CDiscovery::CDiscovery(QObject *parent) :
	QObject( parent ),
	m_bSaved( true ),
	m_nLastID( 0 )

{
}

/**
 * @brief ~CDiscovery: Destructor. Make sure you have stopped the magager befor destroying it.
 */
CDiscovery::~CDiscovery()
{
	Q_ASSERT( m_mServices.empty() );

	// TODO: Watch this closely: The Discovery thread might not like it to be deleted while still running...
}

/**
 * @brief count allows you to access the number of working services for a given network.
 * Locking: YES (synchronous)
 * @return the number of services for the specified network. If no type is specified or the type is
 * null, the total number of all services is returned, no matter whether they are working or not.
 */
quint32	CDiscovery::count(const CNetworkType& oType)
{
	QMutexLocker l( &m_pSection );

	if ( oType.isNull() )
	{
		return m_mServices.size();
	}
	else
	{
		quint16 nCount = 0;
		TServicePtr pService;

		foreach ( TMapPair pair, m_mServices )
		{
			pService = pair.second;

			// we do need a read lock here because we're probably not within the discovery thread
			pService->m_oRWLock.lockForRead();

			// Count all services that have the correct type, are not blocked and have a rating > 0
			if ( pService->m_oNetworkType.isNetwork( oType ) &&
				 !pService->m_bBanned &&
				 pService->m_nRating )
			{
				++nCount;
			}

			pService->m_oRWLock.unlock();
		}

		return nCount;
	}
}

/**
 * @brief start initializes the Discovery Services Manager. Make sure this is called after QApplication
 * is instantiated.
 * Locking: YES (asynchronous)
 * @return whether loading the services was successful.
 */
void CDiscovery::start()
{
	moveToThread( &m_oDiscoveryThread );
	m_oDiscoveryThread.start( QThread::LowPriority );

	QMetaObject::invokeMethod( this, "asyncStartUpHelper", Qt::QueuedConnection );
}

/**
 * @brief stop prepares the Discovery Services Manager for destruction.
 * Locking: YES (synchronous)
 * @return true if the services have been successfully written to disk.
 */
bool CDiscovery::stop()
{
	bool bSaved = save( true );
	clear();

	return bSaved;
}

/**
 * @brief save saves all discovery services to disk, if there have been important modifications to at
 * least one service or bForceSaving is set to true.
 * Locking: YES (synchronous/asynchronous)
 * @param bForceSaving: Set this to true to force saving even if there have been no important service
 * modifications, for example to make sure the hosts from the current session are saved properly.
 * Setting this parameter will also result in synchronous execution. If this parameter is set to false
 * (or omitted), saving will be done asynchronously.
 * @return true if saving to file is known to have been successful; false otherwise (e.g. error/unknown)
 */
bool CDiscovery::save(bool bForceSaving)
{
	if ( bForceSaving )
	{
		// synchronous saving required
		return asyncSyncSavingHelper();
	}
	else
	{
		m_pSection.lock();
		if ( m_bSaved )
		{
			m_pSection.unlock();
			return true; // Saving not required ATM.
		}
		else
		{
			m_pSection.unlock();
			QMetaObject::invokeMethod( this, "asyncSyncSavingHelper", Qt::QueuedConnection );

			// We do not know whether it was successful, so pretend it was not to be on the safe side.
			return false;
		}
	}
}

/**
 * @brief add adds a new Service with a given URL to the manager.
 * Locking: YES (synchronous)
 * @param sURL
 * @param eSType
 * @param oNType
 * @param nRating
 * @return the service ID used to identify the service internally; 0 if the service has not been added.
 */
TServiceID CDiscovery::add(QString sURL, const TServiceType eSType,
							 const CNetworkType& oNType, const quint8 nRating)
{
	// First check whether the URL can be parsed at all.
	// TODO: This might need refining for certain service types.
	QUrl oURL( sURL, QUrl::StrictMode );
	if ( !oURL.isValid() )
	{
		systemLog.postLog( LogSeverity::Error,
						   m_sMessage
						   + tr( "Error: Could not add invalid URL as a discovery service: " )
						   + sURL );
		return 0;
	}

	// Then, normalize the fully encoded URL
	sURL = oURL.toString();
	normalizeURL( sURL );

	TServicePtr pService( CDiscoveryService::createService( sURL, eSType, oNType, nRating ) );

	m_pSection.lock();

	if ( add( pService ) )
	{
		// make sure to return the right ID
		TServiceID nTmp = m_nLastID; // m_nLastID has been set to correct value within add( pService )
		m_pSection.unlock();

		systemLog.postLog( LogSeverity::Notice,
						   m_sMessage + tr( "Notice: New discovery service added: " ) + sURL );

		// inform GUI about new service
		emit serviceAdded( pService );
		return nTmp;
	}
	else // Adding the service failed fore some reason. Most likely the service was invalid or a duplicate.
	{
		m_pSection.unlock();

		systemLog.postLog( LogSeverity::Error,
						   m_sMessage + tr( "Error adding service." ) );
		return 0;
	}
}

/**
 * @brief remove removes a service by ID.
 * Locking: YES (synchronous)
 * @param nID
 * @return true if the removal was successful (e.g. the service could be found), false otherwise.
 */
bool CDiscovery::remove(TServiceID nID)
{
	if ( !nID )
	{
		systemLog.postLog( LogSeverity::Error,
						   m_sMessage + tr( "Internal error: Got request to remove invalid ID: " ) + nID );
		return false; // invalid ID
	}

	m_pSection.lock();

	TIterator iService = m_mServices.find( nID );

	if ( iService == m_mServices.end() )
	{
		m_pSection.unlock();

		systemLog.postLog( LogSeverity::Error,
						   m_sMessage + tr( "Internal error: Got request to remove invalid ID: " ) + nID );
		return false; // Unable to find service by ID
	}

	// inform GUI about service removal
	emit serviceRemoved( nID );

	TServicePtr pService = (*iService).second;

	systemLog.postLog( LogSeverity::Notice,
					   m_sMessage
					   + tr( "Removing discovery service: " )
					   + pService->m_oServiceURL.toString() );

	// stop it if necessary
	pService->cancelRequest();

	// remove it
	m_mServices.erase( nID );

	// Make sure to reassign the now unused ID.
	if ( --nID < m_nLastID )
		m_nLastID = nID;

	m_pSection.unlock();
	return true;
}

/**
 * @brief clear removes all services from the manager.
 * Locking: YES (synchronous)
 * @param bInformGUI: Set this to true if the GUI shall be informed about the removal of the services.
 * The default value is false, which represents the scenario on shutdown, where the GUI will be removed
 * anyway shortly.
 */
void CDiscovery::clear(bool bInformGUI)
{
	m_pSection.lock();

	if ( bInformGUI )
	{
		foreach ( TMapPair pair, m_mServices )
		{
			emit serviceRemoved( pair.second->m_nID ); // inform GUI
		}
	}

	m_mServices.clear();	// TServicePtr takes care of service deletion
	m_nLastID = 0;			// make sure to recycle the IDs.

	m_pSection.unlock();
}

/**
 * @brief check verifies whether the given service is managed by the manager.
 * Locking: YES (synchronous)
 * @return true if managed; false otherwise
 */
bool CDiscovery::check(const TConstServicePtr pService)
{
	QMutexLocker l( &m_pSection );

	TConstIterator iService = m_mServices.find( pService->m_nID );

	if ( iService == m_mServices.end() )
		return false; // Unable to find service by ID

	const TConstServicePtr pExService = (*iService).second;

	if ( *pExService == *pService )
		return true;
	else
		return false;
}

/**
 * @brief CDiscovery::requestNAMgr provides a shared pointer to the discovery services network access
 * manager. Note that the caller needs to hold his copy of the shared pointer until he has finished his
 * network operation to prevent the access manager from being deleted too early.
 * Locking: YES (synchronous)
 * @return
 */
QSharedPointer<QNetworkAccessManager> CDiscovery::requestNAM()
{
	QMutexLocker l( &m_pSection );

	QSharedPointer<QNetworkAccessManager> pReturnVal = m_pNetAccessMgr.toStrongRef();
	if ( !pReturnVal )
	{
		// else create a new access manager (will be deleted if nobody is using it anymore)
		pReturnVal = QSharedPointer<QNetworkAccessManager>( new QNetworkAccessManager(),
															&QObject::deleteLater );
		m_pNetAccessMgr = pReturnVal.toWeakRef();
	}

	return pReturnVal;
}

/**
 * @brief requestServiceList can be used to obtain a complete list of all currently managed services.
 * Connect to the serviceInfo() signal to recieve them.
 * Locking: YES (asynchronous)
 */
void CDiscovery::requestServiceList()
{
	QMetaObject::invokeMethod( this, "asyncRequestServiceListHelper", Qt::QueuedConnection );
}

/**
 * @brief updateService updates a service for a given network type with our IP. Note that not all
 * service types might support or require such updates.
 * Locking: YES (asynchronous)
 * @param type
 */
void CDiscovery::updateService(const CNetworkType& type)
{
	QMetaObject::invokeMethod( this,
							   "asyncUpdateServiceHelper",
							   Qt::QueuedConnection,
							   Q_ARG( const CNetworkType, type ) );
}

void CDiscovery::updateService(TServiceID nID)
{
	QMetaObject::invokeMethod( this,
							   "asyncUpdateServiceHelper",
							   Qt::QueuedConnection,
							   Q_ARG( TServiceID, nID ) );
}

/**
 * @brief queryService queries a service for a given network to obtain hosts to connect to.
 * Locking: YES (asynchronous)
 * @param type
 */
void CDiscovery::queryService(const CNetworkType& type)
{
	QMetaObject::invokeMethod( this,
							   "asyncQueryServiceHelper",
							   Qt::QueuedConnection,
							   Q_ARG( const CNetworkType, type ) );
}

void CDiscovery::queryService(TServiceID nID)
{
	QMetaObject::invokeMethod( this,
							   "asyncQueryServiceHelper",
							   Qt::QueuedConnection,
							   Q_ARG( TServiceID, nID ) );
}

bool CDiscovery::asyncSyncSavingHelper()
{
	QMutexLocker l( &m_pSection );

	systemLog.postLog( LogSeverity::Notice, m_sMessage
					   + tr( "Saving Discovery Services Manager state." ) );

	QString sPath          = quazaaSettings.Discovery.DataPath + "discovery.dat";
	QString sBackupPath    = quazaaSettings.Discovery.DataPath + "discovery_backup.dat";
	QString sTemporaryPath = sBackupPath + "_tmp";

	if ( QFile::exists( sTemporaryPath ) && !QFile::remove( sTemporaryPath ) )
	{
		systemLog.postLog( LogSeverity::Error, m_sMessage
						   + tr( "Error: Could not free space required for data backup: " ) + sPath );
		return false;
	}

	QFile oFile( sTemporaryPath );

	if ( !oFile.open( QIODevice::WriteOnly ) )
	{
		systemLog.postLog( LogSeverity::Error,
						   m_sMessage
						   + tr( "Error: Could open data file for write: " )
						   + sTemporaryPath );
		return false;
	}

	quint16 nVersion = DISCOVERY_CODE_VERSION;

	try
	{
		QDataStream fsFile( &oFile );

		fsFile << nVersion;
		fsFile << count();

		// write services to stream
		foreach (  TMapPair pair, m_mServices )
		{
			CDiscoveryService::save( pair.second.data(), fsFile );
		}
	}
	catch ( ... )
	{
		systemLog.postLog( LogSeverity::Error,
						   m_sMessage + tr( "Error while writing discovery services to disk." ) );
		return false;
	}

	m_bSaved = true;

	l.unlock();

	oFile.close();

	if ( QFile::exists( sPath ) )
	{
		if ( !QFile::remove( sPath ) )
		{
			systemLog.postLog( LogSeverity::Error,
							   m_sMessage + tr( "Error: Could not remove old data file: " ) + sPath );
			return false;
		}

		if ( !QFile::rename( sTemporaryPath, sPath ) )
		{
			systemLog.postLog( LogSeverity::Error,
							   m_sMessage + tr( "Error: Could not rename data file: " ) + sPath );
			return false;
		}
	}

	if ( QFile::exists( sBackupPath ) && !QFile::remove( sBackupPath ) )
	{
		systemLog.postLog( LogSeverity::Warning,
						   m_sMessage + tr( "Error: Could not remove old backup file: " ) + sBackupPath );
	}

	if ( !QFile::copy( sPath, sBackupPath ) )
	{
		systemLog.postLog( LogSeverity::Warning, m_sMessage
						   + tr( "Warning: Could not create create new backup file: " ) + sBackupPath );
	}

	return true;
}

void CDiscovery::asyncStartUpHelper()
{
	// Initialize random number generator.
	qsrand ( QDateTime::currentDateTime().toTime_t() );

	// Load rules from disk.
	QMutexLocker l( &m_pSection );

	// reg. meta types
	qRegisterMetaType<TServiceID>( "TServiceID" );
	qRegisterMetaType<TConstServicePtr>( "TConstServicePtr" );

	m_sMessage = tr( "[Discovery] " );
	load();
}

void CDiscovery::asyncRequestServiceListHelper()
{
	m_pSection.lock();
	foreach ( TMapPair pair, m_mServices )
	{
		emit serviceInfo( pair.second );
	}
	m_pSection.unlock();
}

void CDiscovery::asyncUpdateServiceHelper(const CNetworkType type)
{
	QSharedPointer<QNetworkAccessManager> pNAM = requestNAM();

	if ( pNAM->networkAccessible() == QNetworkAccessManager::Accessible )
	{
		m_pSection.lock();
		TServicePtr pService = getRandomService( type );
		m_pSection.unlock();

		if ( pService )
		{
			systemLog.postLog( LogSeverity::Notice,
							   m_sMessage + tr( "Updating service: " ) + pService->url() );

			pService->update();
		}
		else
		{
			systemLog.postLog( LogSeverity::Warning, m_sMessage
							   + tr( "Unable to update service for network: " ) + type.toString() );

			// TODO: Act accordingly and try to fix the issue
		}
	}
	else
	{
		systemLog.postLog( LogSeverity::Error, m_sMessage
		+ tr( "Could not update service because the network connection is currently unavailable." ) );
	}
}

void CDiscovery::asyncUpdateServiceHelper(TServiceID nID)
{
	// We do not prevent users from manually querying services even if the network connection is reported
	// to be down. Maybe they know better than we do.

	m_pSection.lock();

	TIterator iService = m_mServices.find( nID );

	if ( iService == m_mServices.end() )
	{
		m_pSection.unlock();

		// Unable to find service by ID (should never happen)
		Q_ASSERT( false );

		return;
	}

	TServicePtr pService = (*iService).second;
	m_pSection.unlock();

	Q_ASSERT( pService ); // we should always get a valid service from the iterator

	systemLog.postLog( LogSeverity::Notice, m_sMessage
						   + tr( "Updating service: " ) + pService->url() );

	pService->update();
}

void CDiscovery::asyncQueryServiceHelper(const CNetworkType type)
{
	QSharedPointer<QNetworkAccessManager> pNAM = requestNAM();

	if ( pNAM->networkAccessible() == QNetworkAccessManager::Accessible )
	{
		m_pSection.lock();
		TServicePtr pService = getRandomService( type );
		m_pSection.unlock();

		if ( pService )
		{
			systemLog.postLog( LogSeverity::Notice,
							   m_sMessage + tr( "Querying service: " ) + pService->url() );

			pService->query();
		}
		else
		{
			systemLog.postLog( LogSeverity::Warning, m_sMessage
							   + tr( "Unable to query service for network: " ) + type.toString() );

			// TODO: Act accordingly and try to fix the issue
		}
	}
	else
	{
		systemLog.postLog( LogSeverity::Error, m_sMessage
		+ tr( "Could not query service because the network connection is currently unavailable." ) );
	}
}

void CDiscovery::asyncQueryServiceHelper(TServiceID nID)
{
	// We do not prevent users from manually querying services even if the network connection is reported
	// to be down. Maybe they know better than we do.

	m_pSection.lock();

	TIterator iService = m_mServices.find( nID );

	if ( iService == m_mServices.end() )
	{
		m_pSection.unlock();

		// Unable to find service by ID (should never happen)
		Q_ASSERT( false );

		return; // Unable to find service by ID
	}

	TServicePtr pService = (*iService).second;
	m_pSection.unlock();

	Q_ASSERT( pService ); // we should always get a valid service from the iterator

	systemLog.postLog( LogSeverity::Notice,
					   m_sMessage + tr( "Querying service: " ) + pService->url() );

	pService->query();
}

/**
 * @brief load retrieves stored services from the HDD.
 * Requires locking: YES
 * @return true if loading from file was successful; false otherwise.
 */
void CDiscovery::load()
{
	QString sPath = quazaaSettings.Discovery.DataPath + "discovery.dat";

	if ( load( sPath ) )
	{
		systemLog.postLog( LogSeverity::Debug,
						   m_sMessage + tr( "Loading discovery services from file: " ) + sPath );
		return;
	}
	else // Unable to load default file. Switch to backup one instead.
	{
		sPath = quazaaSettings.Discovery.DataPath + "discovery_backup.dat";

		systemLog.postLog( LogSeverity::Warning, m_sMessage
		+ tr( "Failed to load discovery services from primary file. Switching to backup: " ) + sPath );

		if ( !load( sPath ) )
		{
			systemLog.postLog( LogSeverity::Error,
							   m_sMessage + tr( "Failed to load discovery services!" ) );
		}
	}
}

bool CDiscovery::load( QString sPath )
{
	QFile oFile( sPath );

	if ( ! oFile.open( QIODevice::ReadOnly ) )
		return false;

	CDiscoveryService* pService = nullptr;

	try
	{
		clear();

		QDataStream fsFile( &oFile );

		quint16 nVersion;
		quint32 nCount;

		fsFile >> nVersion;
		fsFile >> nCount;

		QMutexLocker l( &m_pSection );
		while ( nCount > 0 )
		{
			CDiscoveryService::load( pService, fsFile, nVersion );

			add( TServicePtr( pService ) );
			pService = nullptr;
			--nCount;
		}
	}
	catch ( ... )
	{
		if ( pService )
			delete pService;

		clear();
		oFile.close();

		systemLog.postLog( LogSeverity::Error, m_sMessage
						   + tr( "Error: Caught an exception while loading services from file!" ) );

		return false;
	}
	oFile.close();

	return true;
}

/**
 * @brief add... obvious... Note: if a duplicate is detected, the CDiscoveryService passed to the
 * method is deleted within.
 * Requires locking: YES
 * @param pService
 * @return true if the service was added; false if not (e.g. duplicate was detected).
 */
bool CDiscovery::add(TServicePtr& pService)
{
	// TODO: implement bans overwriting existing services

	if ( !pService )
		return false;

	if ( manageDuplicates( pService ) )
	{
		return false;
	}

	// check for already existing ID
	if ( pService->m_nID && m_mServices.find( pService->m_nID ) != m_mServices.end() )
	{
		// We need to assign a new ID, as the previous ID of this service is already in use.

		// This should not happen as the previous services should be loaded into the manager before
		// any new services that might be added during the session.
		Q_ASSERT( false );

		pService->m_nID = 0; // set ID to invalid.
	}

	// assign valid ID if necessary
	if ( !pService->m_nID )
	{
		TConstIterator i;
		do
		{
			// Check the next ID until a free one is found.
			i = m_mServices.find( ++m_nLastID );
		}
		while ( i != m_mServices.end() );

		// assign ID to service
		pService->m_nID = m_nLastID;
	}

	// push to map
	m_mServices[pService->m_nID] = pService;
	return true;
}

// Note: When modifying this method, compatibility to Shareaza should be maintained.
void CDiscovery::addDefaults()
{
	QFile oFile( qApp->applicationDirPath() + "\\DefaultServices.dat" );

	systemLog.postLog( LogSeverity::Debug, m_sMessage + tr( "Loading default services from file." ) );

	if ( !oFile.open( QIODevice::ReadOnly ) )
	{
		systemLog.postLog( LogSeverity::Error,
						   m_sMessage + tr( "Error: Could not open file: " ) + "DefaultServices.dat" );
		return;
	}

	try
	{
		QTextStream fsFile( &oFile );
		QString     sLine, sService;
		QChar       cType;

		QMutexLocker l( &m_pSection );

		while( !fsFile.atEnd() )
		{
			sLine = fsFile.readLine();

			if ( sLine.length() < 7 )	// Blank comment line
				continue;

			cType = sLine.at( 0 );
			sService = sLine.right( sLine.length() - 2 );

			switch( cType.toLatin1() )
			{
			case '1':	// G1 service
				break;
			case '2':	// G2 service
				add( sService, stGWC, CNetworkType( dpG2 ), DISCOVERY_MAX_PROBABILITY );
				break;
			case 'M':	// Multi-network service
				add( sService, stGWC, CNetworkType( dpG2 ), DISCOVERY_MAX_PROBABILITY );
				break;
			case 'D':	// eDonkey service
				break;
			case 'U':	// Bootstrap and UDP Discovery Service
				break;
			case 'X':	// Blocked service
				// TODO: implement class for permanently banned services with unknown type
				add( sService, stNull, CNetworkType( dpNull ), 0 );
				break;
			default:	// Comment line or unsupported
				break;
			}
		}
	}
	catch ( ... )
	{
		systemLog.postLog( LogSeverity::Error,
						   m_sMessage + tr( "Error while loading default servers from file." ) );
	}

	oFile.close();
}

/**
 * @brief manageDuplicates checks if an identical (or very similar) service is alreads present in the
 * manager, decides which service to remove and frees unnecessary data.
 * Requires locking: YES
 * @param pService
 * @return true if a duplicate was detected; pService is cleared in that case. false otherwise.
 */
bool CDiscovery::manageDuplicates(TServicePtr& pService)
{
	QString sURL = pService->m_oServiceURL.toString();

	foreach ( TMapPair pair, m_mServices )
	{
		// already existing service
		TServicePtr pExService = pair.second;
		QString sExURL = pExService->m_oServiceURL.toString();

		// check for services with the same URL; Note: all URLs should be case insesitive due to
		// normalizeURLs(), so case sensitivity is not a problem here.
		if ( !sExURL.compare( sURL ) )
		{
			if ( pService->m_nServiceType == pExService->m_nServiceType )
			{
				// Make sure the existing service is set to handle the networks pService handles, too.
				// (90% of the time, this should be the case anyway, but this is more efficient than
				// using an if statement to test the condition.)
				pExService->m_oNetworkType.setNetwork( pService->m_oNetworkType );

				systemLog.postLog( LogSeverity::Debug, m_sMessage
				+ tr( "Detected a duplicate service. Not going to add the new one." ) );

				pService.clear();
				return true;
			}
			else // This should not happen. Two services with the same URL should be of the same type.
			{
				// Generate nice assert so that this can be analized.
				QString sError = "Services of type %1 and %2 detected sharing the same URL: ";
				sError.arg( pService->type(), pExService->type() );
				sError.append( sURL );
				Q_ASSERT_X( false, "manageDuplicates", sError.toLatin1().data() );
			}
		}

		// check for services with only part of the URL of an other service
		// this filters out "www.example.com"/"www.example.com/gwc.php" pairs
		if ( sExURL.startsWith( sURL ) )
		{
// TODO: implement
		}
		else if ( sURL.startsWith( sExURL ) )
		{
// TODO: implement
		}
	}

	return false;
}

/**
 * @brief normalizeURL transforms a given URL string into a standard form to easa the detection of
 * duplicates, filter out websites caching a service etc.
 * Requires locking: NO
 * @param sURL
 */
void CDiscovery::normalizeURL(QString& sURL)
{
	sURL = sURL.toLower();

	// TODO: Implement


	/*// Check it has a valid protocol
	if ( _tcsnicmp( pszAddress, _T("http://"),  7 ) == 0 )
		pszAddress += 7;
	else if ( _tcsnicmp( pszAddress, _T("https://"), 8 ) == 0 )
		pszAddress += 8;
	else if ( _tcsnicmp( pszAddress, _T("gnutella1:host:"), 15 ) == 0 )
		return TRUE;
	else if ( _tcsnicmp( pszAddress, _T("gnutella2:host:"), 15 ) == 0 )
		return TRUE;
	else if ( _tcsnicmp( pszAddress, _T("uhc:"), 4 ) == 0 )
		return TRUE;
	else if ( _tcsnicmp( pszAddress, _T("ukhl:"), 5 ) == 0 )
		return TRUE;
	else
		return FALSE;*/
}

/**
 * @brief getRandomService: Helper method. Allows to get a random service for a specified network.
 * Requires locking: YES
 * @param oNType
 * @return A discovery service for the specified network; NULL if no working service could be found
 * for the specified network.
 */
CDiscovery::TServicePtr CDiscovery::getRandomService(const CNetworkType& oNType)
{
	TDiscoveryServicesList list;
	quint16 nTotalRating = 0;		// Used to store accumulative rating of all services
									// taken under consideration as return value.
	TServicePtr pService;
	quint32 tNow = static_cast< quint32 >( QDateTime::currentDateTimeUtc().toTime_t() );

	foreach ( TMapPair pair, m_mServices )
	{
		pService = pair.second;

		if ( pService->m_bBanned )
			continue; // skip banned services

		bool bRatingEnabled = pService->m_nRating;

		if ( !bRatingEnabled )
		{
			if ( pService->m_tLastQueried + quazaaSettings.Discovery.ZeroRatingRevivalInterval > tNow )
			{
				// Revive service
				pService->setRating( DISCOVERY_MAX_PROBABILITY );
				++pService->m_nZeroRevivals;
				bRatingEnabled = true;
			}
		}

		// Consider all services that...
		if ( pService->m_oNetworkType.isNetwork( oNType ) &&     // have the correct type
			 bRatingEnabled &&              // have a rating > 0 or are considered for revival
			 pService->m_tLastQueried
			 + quazaaSettings.Discovery.AccessThrottle > tNow && // are not recently used
			 !pService->m_bRunning )                             // are not in use
		{
			list.push_back( pService );
			nTotalRating += pService->m_nRating;
		}
	}

	if ( list.empty() )
	{
		return TServicePtr();
	}
	else
	{
		// Make sure our selection is within [1 ; nTotalRating]
		quint16 nSelectedRating = ( qrand() % nTotalRating ) + 1;
		TListIterator   current = list.begin();
		TServicePtr   pSelected = *current;

		// Iterate threw list until the selected service has been found.
		while ( nSelectedRating > pSelected->m_nRating )
		{
			nSelectedRating -= pSelected->m_nRating;
			pSelected = *( ++current );
		}

		return pSelected;
	}
}
