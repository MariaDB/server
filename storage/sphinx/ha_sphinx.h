//
// $Id: ha_sphinx.h 4818 2014-09-24 08:53:38Z tomat $
//

#if MYSQL_VERSION_ID>=50515
#define TABLE_ARG	TABLE_SHARE
#elif MYSQL_VERSION_ID>50100
#define TABLE_ARG	st_table_share
#else
#define TABLE_ARG	st_table
#endif


#if MYSQL_VERSION_ID>=50120
typedef uchar byte;
#endif


/// forward decls
class THD;
struct CSphReqQuery;
struct CSphSEShare;
struct CSphSEAttr;
struct CSphSEStats;
struct CSphSEThreadTable;

/// Sphinx SE handler class
class ha_sphinx final : public handler
{
protected:
	THR_LOCK_DATA	m_tLock;				///< MySQL lock

	CSphSEShare *	m_pShare;				///< shared lock info

	uint			m_iMatchesTotal;
	uint			m_iCurrentPos;
	const byte *	m_pCurrentKey;
	uint			m_iCurrentKeyLen;

	char *			m_pResponse;			///< searchd response storage
	char *			m_pResponseEnd;			///< searchd response storage end (points to wilderness!)
	char *			m_pCur;					///< current position into response
	bool			m_bUnpackError;			///< any errors while unpacking response

public:
#if MYSQL_VERSION_ID<50100
					ha_sphinx ( TABLE_ARG * table_arg ); // NOLINT
#else
					ha_sphinx ( handlerton * hton, TABLE_ARG * table_arg );
#endif
					~ha_sphinx ();

	const char *	table_type () const override		{ return "SPHINX"; }	///< SE name for display purposes
	const char *	index_type ( uint ) override		{ return "HASH"; }		///< index type name for display purposes

	#if MYSQL_VERSION_ID>50100
	ulonglong		table_flags () const override	{ return HA_CAN_INDEX_BLOBS | 
                                                                 HA_CAN_TABLE_CONDITION_PUSHDOWN; } ///< bitmap of implemented flags (see handler.h for more info)
	#else
	ulong			table_flags () const	{ return HA_CAN_INDEX_BLOBS; }			///< bitmap of implemented flags (see handler.h for more info)
	#endif

	ulong			index_flags ( uint, uint, bool ) const override	{ return 0; }	///< bitmap of flags that says how SE implements indexes
	uint			max_supported_record_length () const override	{ return HA_MAX_REC_LENGTH; }
	uint			max_supported_keys () const override				{ return 1; }
	uint			max_supported_key_parts () const override		{ return 1; }
	uint			max_supported_key_length () const override		{ return MAX_KEY_LENGTH; }
	uint			max_supported_key_part_length () const override	{ return MAX_KEY_LENGTH; }

	IO_AND_CPU_COST	scan_time () override
	{
          IO_AND_CPU_COST cost;
          cost.io= 0;
          cost.cpu= (double) (stats.records+stats.deleted) * DISK_READ_COST;
          return cost;
        }
        IO_AND_CPU_COST keyread_time(uint index, ulong ranges, ha_rows rows,
                                    ulonglong blocks) override
	{
          IO_AND_CPU_COST cost;
          cost.io= ranges;
          cost.cpu= 0;
          return cost;
        }
        IO_AND_CPU_COST rnd_pos_time(ha_rows rows) override
	{
          IO_AND_CPU_COST cost;
          cost.io= 0;
          cost.cpu= 0;
          return cost;
        }

public:
	int				open ( const char * name, int mode, uint test_if_locked ) override;
	int				close () override;

	int				write_row ( const byte * buf ) override;
	int				update_row ( const byte * old_data, const byte * new_data ) override;
	int				delete_row ( const byte * buf ) override;
	int				extra ( enum ha_extra_function op ) override;

	int				index_init ( uint keynr, bool sorted ) override; // 5.1.x
	int				index_init ( uint keynr ) { return index_init ( keynr, false ); } // 5.0.x

	int				index_end () override;
	int				index_read ( byte * buf, const byte * key, uint key_len, enum ha_rkey_function find_flag ) override;
	int				index_read_idx ( byte * buf, uint idx, const byte * key, uint key_len, enum ha_rkey_function find_flag );
	int				index_next ( byte * buf ) override;
	int				index_next_same ( byte * buf, const byte * key, uint keylen ) override;
	int				index_prev ( byte * buf ) override;
	int				index_first ( byte * buf ) override;
	int				index_last ( byte * buf ) override;

	int				get_rec ( byte * buf, const byte * key, uint keylen );

	int				rnd_init ( bool scan ) override;
	int				rnd_end () override;
	int				rnd_next ( byte * buf ) override;
	int				rnd_pos ( byte * buf, byte * pos ) override;
	void			position ( const byte * record ) override;

#if MYSQL_VERSION_ID>=50030
	int				info ( uint ) override;
#else
	void			info ( uint );
#endif

	int				reset() override;
	int				external_lock ( THD * thd, int lock_type ) override;
	int				delete_all_rows () override;
	ha_rows			        records_in_range ( uint inx, const key_range * min_key, const key_range * max_key,  page_range *pages) override;

	int				delete_table ( const char * from ) override;
	int				rename_table ( const char * from, const char * to ) override;
	int				create ( const char * name, TABLE * form, HA_CREATE_INFO * create_info ) override;

	THR_LOCK_DATA **		store_lock ( THD * thd, THR_LOCK_DATA ** to, enum thr_lock_type lock_type ) override;

public:
#if MYSQL_VERSION_ID<50610
	virtual const COND *	cond_push ( const COND *cond );
#else
	const Item *		cond_push ( const Item *cond ) override;
#endif	
	void			cond_pop () override;

private:
	uint32			m_iFields;
	char **			m_dFields;

	uint32			m_iAttrs;
	CSphSEAttr *	m_dAttrs;
	int				m_bId64;

	int *			m_dUnboundFields;

private:
	int				Connect ( const char * sQueryHost, ushort uPort );
	int				ConnectAPI ( const char * sQueryHost, int iQueryPort );
	int				HandleMysqlError ( struct st_mysql * pConn, int iErrCode );

	uint32			UnpackDword ();
	char *			UnpackString ();
	bool			UnpackSchema ();
	bool			UnpackStats ( CSphSEStats * pStats );
	bool			CheckResponcePtr ( int iLen );

	CSphSEThreadTable *	GetTls ();
};


#if MYSQL_VERSION_ID < 50100
bool sphinx_show_status ( THD * thd );
#endif

//
// $Id: ha_sphinx.h 4818 2014-09-24 08:53:38Z tomat $
//
