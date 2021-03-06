/*  S4 - An XMMS2 medialib backend
 *  Copyright (C) 2009, 2010 Sivert Berg
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2.1 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 */

#include "s4_priv.h"
#include "logging.h"
#include <string.h>
#include <stdlib.h>

#ifdef _WIN32
#include <io.h>      /* For _chsize */
#include <Windows.h> /* For (Un)LockFile */
#else
#include <unistd.h>  /* For ftruncate */
#include <fcntl.h>
#endif

/**
 * @defgroup Log Log
 * @ingroup S4
 * @brief Logs every action so we can redo changes if something crashes.
 *
 * @{
 */

typedef enum {
	LOG_ENTRY_ADD = 0xaddadd,
	LOG_ENTRY_DEL = 0xde1e7e,
	LOG_ENTRY_WRAP = 0x123123,
	LOG_ENTRY_INIT = 0x87654321,
	LOG_ENTRY_BEGIN = 0x1,
	LOG_ENTRY_END = 0x2,
	LOG_ENTRY_WRITING = 0x3,
	LOG_ENTRY_CHECKPOINT = 0x4
} log_type_t;

#define LOG_SIZE (2*1024*1024)

struct log_header {
	log_type_t type;
	log_number_t num;
};

struct mod_header {
	int32_t ka_len;
	int32_t va_len;
	int32_t kb_len;
	int32_t vb_len;
	int32_t s_len;
};

struct s4_log_data_St {
	FILE *logfile;
	int log_users;
	GMutex lock;

	log_number_t last_checkpoint;
	log_number_t last_synced;
	log_number_t last_logpoint;
	log_number_t next_logpoint;
};

s4_log_data_t *_log_create_data ()
{
	s4_log_data_t *ret = calloc (1, sizeof (s4_log_data_t));

	g_mutex_init (&ret->lock);

	return ret;
}

void _log_free_data (s4_log_data_t *data)
{
	g_mutex_clear (&data->lock);
	free (data);
}

/**
 * Calculates the size of a log entry from the header
 * @param hdr The header to calculate the size of.
 * @return The calculated size.
 */
static int _get_size (struct mod_header *hdr)
{
	int ret = sizeof (struct mod_header);

	ret += hdr->ka_len + hdr->kb_len + hdr->s_len;

	if (hdr->va_len == -1)
		ret += sizeof (int32_t);
	else
		ret += hdr->va_len;

	if (hdr->vb_len == -1)
		ret += sizeof (int32_t);
	else
		ret += hdr->vb_len;

	return ret;
}

/**
 * Writes a string to a file.
 * @param str The string to write.
 * @param len The length of the string.
 * @param file The file to write to.
 */
static void _write_str (const char *str, int len, FILE *file)
{
	fwrite (str, 1, len, file);
}

/**
 * Writes a value to a file.
 * @param val The value to write.
 * @param len The lenght of the value.
 * @param file The file to write to.
 */
static void _write_val (const s4_val_t *val, int len, FILE *file)
{
	const char *s;
	int32_t i;

	if (len == -1) {
		s4_val_get_int (val, &i);
		fwrite (&i, sizeof (int32_t), 1, file);
	} else {
		s4_val_get_str (val, &s);
		_write_str (s, len, file);
	}
}

/**
 * Returns the length of a value.
 *
 * @param val The value to get the lenght of.
 * @return If it is a string value it will return the string length.
 * If it is an integer value it will return -1.
 */
static int _get_val_len (const s4_val_t *val)
{
	const char *s;

	if (s4_val_get_str (val, &s)) {
		return strlen (s);
	}
	return -1;
}

/**
 * Estimates the size needed to write the entire oplist to the log.
 *
 * @param list The oplist to estimate the size of
 * @param writing A pointer to an int that will be set to 1
 * if the oplist contains a write.
 * @return The estimated size.
 */
static int _estimate_size (oplist_t *list, int *writing) {
	int ret = 0, largest = 0;
	_oplist_first (list);

	while (_oplist_next (list)) {
		const char *key_a, *key_b, *src;
		const s4_val_t *val_a, *val_b;
		int size = sizeof (struct log_header);

		if (_oplist_get_add (list, &key_a, &val_a, &key_b, &val_b, &src)) {
			size += sizeof (struct mod_header);
			size += strlen (key_a) + strlen (key_a) + strlen (src);
			size += _get_val_len (val_a) + _get_val_len (val_b);
		} else if (_oplist_get_del (list, &key_a, &val_a, &key_b, &val_b, &src)) {
			size += sizeof (struct mod_header);
			size += strlen (key_a) + strlen (key_a) + strlen (src);
			size += _get_val_len (val_a) + _get_val_len (val_b);
		} else if (_oplist_get_writing (list)) {
			/* A write is only the size of a log header */
			*writing = 1;
		}

		largest = MAX (size, largest);
		ret += size;
	}

	if (ret == 0) {
		return 0;
	}

	/* Add the size of begin, end and a warp-around header.
	 * We also add the size of the largest entry as this is the
	 * most extra space that could be needed on a wrap-around.
	 */
	ret += 3 * sizeof (struct log_header) + largest;
	return ret;
}

/**
 * Locks the log.
 * @param s4 The database to lock the log of.
 */
static void _log_lock (s4_t *s4)
{
	g_mutex_lock (&s4->log_data->lock);
}

/**
 * Unlocks the log.
 * @param s4 The database to unlock the log of.
 */
static void _log_unlock (s4_t *s4)
{
	g_mutex_unlock (&s4->log_data->lock);
}


/**
 * Writes a log header to the log.
 *
 * @param s4 The database handle.
 * @param hdr The header to write.
 * @param size The size of the data following the header.
 */
static void _log_write_header (s4_t *s4, struct log_header hdr, int size)
{
	log_number_t pos, round;

	if (s4->log_data->logfile == NULL)
		return;

	pos = s4->log_data->next_logpoint % LOG_SIZE;
	round = s4->log_data->next_logpoint / LOG_SIZE;

	/* Wrap around if we're at the end */
	if ((pos + size) > (LOG_SIZE - sizeof (struct log_header) * 2)) {
		struct log_header hdr;

		hdr.num = pos + round * LOG_SIZE;
		hdr.type = LOG_ENTRY_WRAP;
		fwrite (&hdr, sizeof (struct log_header), 1, s4->log_data->logfile);
		pos = 0;
		round++;
		rewind (s4->log_data->logfile);
	}

	hdr.num = pos + round * LOG_SIZE;
	fwrite (&hdr, sizeof (struct log_header), 1, s4->log_data->logfile);

	s4->log_data->last_logpoint = s4->log_data->next_logpoint;
	s4->log_data->next_logpoint = ftell (s4->log_data->logfile) + round * LOG_SIZE + size;
}

/**
 * Writes a log entry for a modification operation (add or del).
 * @param s4 The database we are writing the log entry to.
 * @param type The log entry type.
 * @param key_a The key_a string.
 * @param val_a The val_a value.
 * @param key_b The key_b string.
 * @param val_b The val_b value.
 * @param src The source string.
 */
static void _log_mod (s4_t *s4, log_type_t type, const char *key_a, const s4_val_t *val_a,
		const char *key_b, const s4_val_t *val_b, const char *src)
{
	struct log_header lhdr;
	struct mod_header mhdr;
	int size;

	if (s4->log_data->logfile == NULL)
		return;

	lhdr.type = type;
	mhdr.ka_len = strlen (key_a);
	mhdr.kb_len = strlen (key_b);
	mhdr.s_len = strlen (src);
	mhdr.va_len = _get_val_len (val_a);
	mhdr.vb_len = _get_val_len (val_b);

	size = _get_size (&mhdr);

	_log_write_header (s4, lhdr, size);

	fwrite (&mhdr, sizeof (struct mod_header), 1, s4->log_data->logfile);

	_write_str (key_a, mhdr.ka_len, s4->log_data->logfile);
	_write_val (val_a, mhdr.va_len, s4->log_data->logfile);
	_write_str (key_b, mhdr.kb_len, s4->log_data->logfile);
	_write_val (val_b, mhdr.vb_len, s4->log_data->logfile);
	_write_str (src, mhdr.s_len, s4->log_data->logfile);
}

/**
 * Writes a single header with no data.
 * @param s4 The database handle.
 * @param type The type of the log header.
 */
static void _log_simple (s4_t *s4, log_type_t type)
{
	struct log_header hdr;

	hdr.type = type;

	_log_write_header (s4, hdr, 0);
}

/**
 * Writes a checkpoint entry to the log, marking that the
 * database has finished being written to disk.
 * @param s4 The database to write the log entry to.
 */
void _log_checkpoint (s4_t *s4)
{
	struct log_header hdr;
	hdr.type = LOG_ENTRY_CHECKPOINT;

	_log_lock (s4);
	_log_simple (s4, LOG_ENTRY_BEGIN);
	_log_write_header (s4, hdr, sizeof (int32_t));
	fwrite (&s4->log_data->last_synced, sizeof (log_number_t), 1, s4->log_data->logfile);
	s4->log_data->last_checkpoint = s4->log_data->last_synced;
	_log_simple (s4, LOG_ENTRY_END);
	_log_unlock (s4);
}

/**
 * Flushes file buffers and syncs the log to disk.
 * @param s4 The database to flush the log of.
 */
static void _log_flush (s4_t *s4)
{
	fflush (s4->log_data->logfile);
	fsync (fileno (s4->log_data->logfile));
}

/**
 * Writes all the operations in an oplist to disk.
 *
 * @param list The oplist to write.
 * @return 0 on error, non-zero on success.
 */
int _log_write (oplist_t *list)
{
	s4_t *s4 = _oplist_get_db (list);
	int writing = 0;
	int size = _estimate_size (list, &writing);

	if (s4->log_data->logfile == NULL || size == 0)
		return 1;

	_log_lock (s4);
	if (writing) {
		s4->log_data->last_synced = s4->log_data->last_logpoint;
	}

	if ((s4->log_data->next_logpoint + size) > (s4->log_data->last_checkpoint + LOG_SIZE)) {
		_log_unlock (s4);
		return writing;
	}

	_log_simple (s4, LOG_ENTRY_BEGIN);

	_oplist_first (list);
	while (_oplist_next (list)) {
		const char *key_a, *key_b, *src;
		const s4_val_t *val_a, *val_b;

		if (_oplist_get_add (list, &key_a, &val_a, &key_b, &val_b, &src)) {
			_log_mod (s4, LOG_ENTRY_ADD, key_a, val_a, key_b, val_b, src);
		} else if (_oplist_get_del (list, &key_a, &val_a, &key_b, &val_b, &src)) {
			_log_mod (s4, LOG_ENTRY_DEL, key_a, val_a, key_b, val_b, src);
		} else if (_oplist_get_writing (list)) {
			_log_simple (s4, LOG_ENTRY_WRITING);
		}
	}

	_log_simple (s4, LOG_ENTRY_END);

	if (s4->log_data->last_synced > (s4->log_data->last_checkpoint + LOG_SIZE / 2))
		_start_sync (s4);

	_log_flush (s4);
	_log_unlock (s4);
	return 1;
}

/**
 * Reads a string from the log file.
 * @param s4 The database
 * @param len The string length.
 * @return A pointer to a constant string, or NULL on error.
 */
static const char *_read_str (s4_t *s4, int len)
{
	const char *ret = NULL;
	char *str = NULL;

	if (len < 0 || len > LOG_SIZE)
		goto cleanup;

	str = malloc (len + 1);

	if (fread (str, 1, len, s4->log_data->logfile) != len)
		goto cleanup;

	str[len] = '\0';
	ret = _string_lookup (s4, str);

cleanup:
	free (str);
	return ret;
}

/**
 * Reads an S4 value from the log file.
 * @param s4 the database.
 * @param len The value length.
 * @return A pointer to a constant value, or NULL on error.
 */
static const s4_val_t *_read_val (s4_t *s4, int len)
{
	const s4_val_t *ret = NULL;

	if (len == -1) {
		int32_t i;
		fread (&i, sizeof (int32_t), 1, s4->log_data->logfile);
		ret = _int_lookup_val (s4, i);
	} else {
		const char *str = _read_str (s4, len);

		if (str != NULL)
			ret = _string_lookup_val (s4, str);
	}

	return ret;
}

/**
 * Reads a modification entry (add or del).
 *
 * @param s4 The database.
 * @param list The oplist to insert the operation in.
 * @param type The type of the log entry.
 * @return 0 on error, non-zero on success.
 */
static int _read_mod (s4_t *s4, oplist_t *list, log_type_t type)
{
	const char *key_a, *key_b, *src;
	const s4_val_t *val_a, *val_b;
	struct mod_header mhdr;

	if (list == NULL)
		return 0;

	fread (&mhdr, sizeof (struct mod_header), 1, s4->log_data->logfile);

	key_a = _read_str (s4, mhdr.ka_len);
	val_a = _read_val (s4, mhdr.va_len);
	key_b = _read_str (s4, mhdr.kb_len);
	val_b = _read_val (s4, mhdr.vb_len);
	src = _read_str (s4, mhdr.s_len);

	if (key_a == NULL || key_b == NULL
			|| val_a == NULL || val_b == NULL
			|| src == NULL) {
		return 0;
	}

	if (type == LOG_ENTRY_ADD) {
		_oplist_insert_add (list, key_a, val_a, key_b, val_b, src);
	} else if (type == LOG_ENTRY_DEL) {
		_oplist_insert_del (list, key_a, val_a, key_b, val_b, src);
	}

	return 1;
}

/**
 * Redoes everything that happened since the last checkpoint
 *
 * @param s4 The database to add changes to
 * @return 0 on error, non-zero otherwise
 */
static int _log_redo (s4_t *s4)
{
	struct log_header hdr;
	log_number_t pos, round, new_checkpoint = -1, new_synced = -1;
	log_number_t last_valid_logpoint;
	oplist_t *oplist = NULL;
	int invalid_entry = 0;

	fflush (s4->log_data->logfile);

	/* Check if the log wrapped around since our last write */
	pos = s4->log_data->last_logpoint % LOG_SIZE;
	if (fseek (s4->log_data->logfile, pos, SEEK_SET) != 0 ||
			fread (&hdr, sizeof (struct log_header), 1, s4->log_data->logfile) != 1) {
		return 0;
	}

	/* If it did, we have to read in everything */
	if (hdr.num != s4->log_data->last_logpoint) {
		_reread_file (s4);
	}

	last_valid_logpoint = s4->log_data->last_logpoint;
	s4->log_data->next_logpoint = s4->log_data->last_logpoint + sizeof (struct log_header);

	pos = s4->log_data->next_logpoint % LOG_SIZE;
	round = s4->log_data->next_logpoint / LOG_SIZE;
	if (fseek (s4->log_data->logfile, pos, SEEK_SET) != 0) {
		return 0;
	}

	/* Read log entries until fread fails, or the header num is different
	 * from the expected number.
	 */
	while (!invalid_entry
			&& fread (&hdr, sizeof (struct log_header), 1, s4->log_data->logfile) == 1
			&& hdr.num == (pos + round * LOG_SIZE)) {

		s4->log_data->last_logpoint = s4->log_data->next_logpoint;

		switch (hdr.type) {
		case LOG_ENTRY_WRAP:
			round++;
			rewind (s4->log_data->logfile);
			break;

		case LOG_ENTRY_DEL:
		case LOG_ENTRY_ADD:
			if (!_read_mod (s4, oplist, hdr.type))
				invalid_entry = 1;

			break;

		case LOG_ENTRY_CHECKPOINT:
			fread (&new_checkpoint, sizeof (log_number_t), 1, s4->log_data->logfile);
			break;

		case LOG_ENTRY_WRITING:
			new_synced = s4->log_data->last_logpoint;
			break;

		case LOG_ENTRY_BEGIN:
			oplist = _oplist_new (_transaction_dummy_alloc (s4));
			new_checkpoint = -1;
			new_synced = -1;
			break;

		case LOG_ENTRY_END:
			if (oplist == NULL) {
				break;
			}

			_oplist_execute (oplist, 0);
			_transaction_dummy_free (_oplist_get_trans (oplist));
			_oplist_free (oplist);
			oplist = NULL;

			if (new_checkpoint != -1) {
				s4->log_data->last_synced = s4->log_data->last_checkpoint = new_checkpoint;
			} else if (new_synced != -1) {
				s4->log_data->last_synced = new_synced;
			}
			last_valid_logpoint = s4->log_data->last_logpoint;
			break;

		case LOG_ENTRY_INIT:
			/* Ignore */
			break;

		default:
			/* Unknown header type */
			invalid_entry = 1;
			break;
		}

		pos = ftell (s4->log_data->logfile);
		s4->log_data->next_logpoint = pos + round * LOG_SIZE;
	}

	if (oplist != NULL) {
		_transaction_dummy_free (_oplist_get_trans (oplist));
		_oplist_free (oplist);
	}

	s4->log_data->last_logpoint = last_valid_logpoint;
	s4->log_data->next_logpoint = last_valid_logpoint + sizeof (struct log_header);
	pos = s4->log_data->next_logpoint % LOG_SIZE;
	fseek (s4->log_data->logfile, pos, SEEK_SET);

	return 1;
}

/**
 * Truncates the logfile to LOG_SIZE length.
 * @param s4 The database to truncate the logfile of.
 */
static void _log_truncate (s4_t *s4)
{
#ifdef _WIN32
	_chsize (fileno (s4->log_data->logfile, LOG_SIZE));
#else
	ftruncate (fileno (s4->log_data->logfile), LOG_SIZE);
#endif
}

/**
 * Locks a byte in the logfile.
 * @param s4 The databae to lock the log of.
 * @param offset The offset of the byte to lock.
 */
static void _log_lockf (s4_t *s4, int offset)
{
#ifdef _WIN32
	while (!LockFile (fileno (s4->log_data->logfile), offset, 0, 1, 0));
#else
	struct flock lock;
	lock.l_type = F_UNLCK;
	lock.l_whence = SEEK_SET;
	lock.l_start = offset;
	lock.l_len = 1;

	while (fcntl (fileno (s4->log_data->logfile), F_SETLKW, &lock) == -1);
#endif
}

/**
 * Unlocks a byte in the logfile.
 * @param s4 The database to unlock the log of.
 * @param offset The offset of the byte to unlock.
 */
static void _log_unlockf (s4_t *s4, int offset)
{
#ifdef _WIN32
	while (!UnlockFile (fileno (s4->log_data->logfile), offset, 0, 1, 0));
#else
	struct flock lock;
	lock.l_type = F_UNLCK;
	lock.l_whence = SEEK_SET;
	lock.l_start = offset;
	lock.l_len = 1;

	while (fcntl (fileno (s4->log_data->logfile), F_SETLKW, &lock) == -1);
#endif
}

/**
 * Opens a log file.
 *
 * @param s4 The database to open the logfile for
 * @return 0 on error, non-zero otherwise
 */
int _log_open (s4_t *s4)
{
	char *log_name = g_strconcat (s4->filename, ".log", NULL);

	s4->log_data->logfile = fopen (log_name, "r+");

	if (s4->log_data->logfile == NULL) {
		s4->log_data->logfile = fopen (log_name, "w+");
		if (s4->log_data->logfile == NULL) {
			s4_set_errno (S4E_LOGOPEN);
			return 0;
		}
		_log_truncate (s4);
		_log_simple (s4, LOG_ENTRY_INIT);
	}
	g_free (log_name);

	return 1;
}

/**
 * Closes the log file.
 * @param s4 The database to close the logfile of.
 * @return 0 on error, non-zero on success.
 */
int _log_close (s4_t *s4)
{
	if (fclose (s4->log_data->logfile) != 0) {
		return 0;
	}
	return 1;
}

/**
 * Locks the log file.
 * It will redo everything new in the log.
 * @param s4 The database to lock the log of.
 */
void _log_lock_file (s4_t *s4)
{
	if (s4->log_data->logfile == NULL)
		return;

	_log_lock (s4);
	if (s4->log_data->log_users == 0) {
		_log_lockf (s4, 0);
		_log_redo (s4);
	}

	s4->log_data->log_users++;
	_log_unlock (s4);
}

/**
 * Unlocks the log file.
 * @param s4 The database to unlock the log of.
 */
void _log_unlock_file (s4_t *s4)
{
	if (s4->log_data->logfile == NULL)
		return;

	_log_lock (s4);
	s4->log_data->log_users--;

	if (s4->log_data->log_users < 0) {
		S4_ERROR ("_log_unlock_file called more time than _log_lock_file!");
		s4->log_data->log_users = 0;
	}
	if (s4->log_data->log_users == 0) {
		_log_unlockf (s4, 0);
	}
	_log_unlock (s4);
}

/**
 * Locks the database file.
 * This is actually a lock set on the log file.
 *
 * @param s4 The database to log.
 */
void _log_lock_db (s4_t *s4)
{
	_log_lockf (s4, 1);
}

/**
 * Unlocks the database file.
 *
 * @param s4 The database to unlock.
 */
void _log_unlock_db (s4_t *s4)
{
	_log_unlockf (s4, 1);
}

log_number_t _log_last_synced (s4_t *s4)
{
	return s4->log_data->last_synced;
}

void _log_init (s4_t *s4, log_number_t last_checkpoint)
{
	s4->log_data->last_synced = last_checkpoint;
	s4->log_data->last_logpoint = last_checkpoint;
	s4->log_data->last_checkpoint = last_checkpoint;
}

/**
 * @}
 */
