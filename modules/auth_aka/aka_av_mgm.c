/*
 * AKA Authentication - generic Authentication Manager support
 *
 * Copyright (C) 2024 Razvan Crainea
 *
 * This file is part of opensips, a free SIP server.
 *
 * opensips is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version
 *
 * opensips is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA
 *
 */

#include "../../ut.h"
#include "../../lib/hash.h"
#include "aka_av_mgm.h"
#include "auth_aka.h"
#include <math.h>

static gen_hash_t *aka_users;
OSIPS_LIST_HEAD(aka_av_managers);

static void aka_av_insert(struct aka_user *user, struct aka_av *av);

/* every cached AV carries the local pending timeout plus a small margin, so
 * that a node which never sees the answer cannot keep it alive forever */
static int aka_cdb_av_ttl;

#define AKA_CDB_KEY_PREFIX "aka_av|"
#define AKA_CDB_KEY_PREFIX_LEN (sizeof(AKA_CDB_KEY_PREFIX) - 1)
#define AKA_CDB_DELIM '|'
#define AKA_CDB_FIELDS 7


int aka_init_mgm(int hash_size, int pending_timeout)
{
	aka_users = hash_init(hash_size);
	if (!aka_users) {
		LM_ERR("cannot create AKA users hash\n");
		return -1;
	}
	aka_cdb_av_ttl = pending_timeout + 5;
	return 0;
}


/* aka_av|<impu>|<impi>|<nonce> - the nonce is base64, the identities are
 * SIP URIs, so none of them can contain the delimiter */
static int aka_cdb_build_key(str *impu, str *impi, str *nonce, str *key)
{
	char *p;

	key->len = AKA_CDB_KEY_PREFIX_LEN + impu->len + 1 + impi->len + 1 + nonce->len;
	key->s = pkg_malloc(key->len);
	if (!key->s) {
		LM_ERR("oom for cachedb key\n");
		return -1;
	}

	p = key->s;
	memcpy(p, AKA_CDB_KEY_PREFIX, AKA_CDB_KEY_PREFIX_LEN);
	p += AKA_CDB_KEY_PREFIX_LEN;
	memcpy(p, impu->s, impu->len);
	p += impu->len;
	*p++ = AKA_CDB_DELIM;
	memcpy(p, impi->s, impi->len);
	p += impi->len;
	*p++ = AKA_CDB_DELIM;
	memcpy(p, nonce->s, nonce->len);
	return 0;
}


/* <state>|<algmask>|<alg>|<nonce>|<xres>|<ck>|<ik>
 * everything but the base64 nonce is opaque to us, so it goes out hex
 * encoded rather than raw - a single stray delimiter byte would otherwise
 * desynchronise the parser on the other node */
static int aka_cdb_serialize_av(struct aka_av *av, str *value)
{
	char *p;

	value->len = snprintf(NULL, 0, "%d%c%d%c%d%c", av->state, AKA_CDB_DELIM,
			av->algmask, AKA_CDB_DELIM, av->alg, AKA_CDB_DELIM) +
		av->authenticate.len + 1 + av->authorize.len * 2 + 1 +
		av->ck.len * 2 + 1 + av->ik.len * 2;

	value->s = pkg_malloc(value->len);
	if (!value->s) {
		LM_ERR("oom for cachedb value\n");
		return -1;
	}

	p = value->s;
	p += sprintf(p, "%d%c%d%c%d%c", av->state, AKA_CDB_DELIM,
			av->algmask, AKA_CDB_DELIM, av->alg, AKA_CDB_DELIM);
	memcpy(p, av->authenticate.s, av->authenticate.len);
	p += av->authenticate.len;
	*p++ = AKA_CDB_DELIM;
	/* string2hex() emits a single '0' for an empty input, which would not
	 * survive the round trip through hex2string() */
	if (av->authorize.len)
		p += string2hex(av->authorize.s, av->authorize.len, p);
	*p++ = AKA_CDB_DELIM;
	if (av->ck.len)
		p += string2hex(av->ck.s, av->ck.len, p);
	*p++ = AKA_CDB_DELIM;
	if (av->ik.len)
		p += string2hex(av->ik.s, av->ik.len, p);

	return 0;
}


static int aka_cdb_split(str *value, str *fields)
{
	char *p = value->s, *end = value->s + value->len, *delim;
	int f, last = AKA_CDB_FIELDS - 1;

	for (f = 0; f < AKA_CDB_FIELDS; f++) {
		if (f == last) {
			fields[f].s = p;
			fields[f].len = end - p;
			break;
		}
		delim = memchr(p, AKA_CDB_DELIM, end - p);
		if (!delim)
			goto malformed;
		fields[f].s = p;
		fields[f].len = delim - p;
		p = delim + 1;
	}

	/* the last field runs to the end of the value, so a delimiter left in
	 * it means we are looking at a format we do not know */
	if (memchr(fields[last].s, AKA_CDB_DELIM, fields[last].len))
		goto malformed;
	return 0;

malformed:
	LM_ERR("malformed cached AV: %.*s\n", value->len, value->s);
	return -1;
}


/* returns a detached AV in shm memory, not linked into any user yet */
static struct aka_av *aka_cdb_deserialize_av(str *value)
{
	str fields[AKA_CDB_FIELDS];
	str *nonce, *xres, *ck, *ik;
	struct aka_av *av;
	int state, algmask, alg;
	char *p;

	if (aka_cdb_split(value, fields) < 0)
		return NULL;

	if (str2sint(&fields[0], &state) < 0 || str2sint(&fields[1], &algmask) < 0 ||
			str2sint(&fields[2], &alg) < 0) {
		LM_ERR("non-numeric header in cached AV: %.*s\n", value->len, value->s);
		return NULL;
	}

	nonce = &fields[3];
	xres = &fields[4];
	ck = &fields[5];
	ik = &fields[6];

	if ((xres->len & 1) || (ck->len & 1) || (ik->len & 1)) {
		LM_ERR("odd-length hex field in cached AV: %.*s\n", value->len, value->s);
		return NULL;
	}

	av = shm_malloc(sizeof(*av) + nonce->len +
			(xres->len + ck->len + ik->len) / 2);
	if (!av) {
		LM_ERR("oom for cached AV\n");
		return NULL;
	}
	memset(av, 0, sizeof(*av));
	av->state = state;
	av->algmask = algmask;
	av->alg = alg;

	p = av->buf;
	av->authenticate.s = p;
	av->authenticate.len = nonce->len;
	memcpy(p, nonce->s, nonce->len);
	p += nonce->len;

	av->authorize.s = p;
	av->authorize.len = xres->len / 2;
	av->ck.s = p + av->authorize.len;
	av->ck.len = ck->len / 2;
	av->ik.s = av->ck.s + av->ck.len;
	av->ik.len = ik->len / 2;

	if (hex2string(xres->s, xres->len, av->authorize.s) < 0 ||
			hex2string(ck->s, ck->len, av->ck.s) < 0 ||
			hex2string(ik->s, ik->len, av->ik.s) < 0) {
		LM_ERR("invalid hex field in cached AV: %.*s\n", value->len, value->s);
		shm_free(av);
		return NULL;
	}

	INIT_LIST_HEAD(&av->list);
	av->ts = av->new_ts = get_ticks();
	return av;
}


int aka_cdb_store_av(str *impu, str *impi, struct aka_av *av)
{
	str key, value;
	int ret = -1;

	if (!aka_cdb)
		return 0;

	if (aka_cdb_build_key(impu, impi, &av->authenticate, &key) < 0)
		return -1;

	if (aka_cdb_serialize_av(av, &value) < 0) {
		pkg_free(key.s);
		return -1;
	}

	if (aka_cdbf.set(aka_cdb, &key, &value, aka_cdb_av_ttl) < 0)
		LM_ERR("could not store AV %.*s\n", key.len, key.s);
	else
		ret = 0;

	pkg_free(key.s);
	pkg_free(value.s);
	return ret;
}


struct aka_av *aka_cdb_fetch_av(str *impu, str *impi, str *nonce)
{
	str key, value = STR_NULL;
	struct aka_av *av;

	if (!aka_cdb)
		return NULL;

	if (aka_cdb_build_key(impu, impi, nonce, &key) < 0)
		return NULL;

	if (aka_cdbf.get(aka_cdb, &key, &value) < 0 || !value.s) {
		LM_DBG("no cached AV for %.*s\n", key.len, key.s);
		pkg_free(key.s);
		return NULL;
	}

	av = aka_cdb_deserialize_av(&value);
	pkg_free(key.s);
	pkg_free(value.s);
	return av;
}


int aka_cdb_remove_av(str *impu, str *impi, str *nonce)
{
	str key;
	int ret;

	if (!aka_cdb)
		return 0;

	if (aka_cdb_build_key(impu, impi, nonce, &key) < 0)
		return -1;

	ret = aka_cdbf.remove(aka_cdb, &key);
	pkg_free(key.s);
	return ret;
}


struct aka_av_mgm *aka_get_mgm(str *name)
{
	struct list_head *it;
	struct aka_av_mgm *mgm;
	list_for_each(it, &aka_av_managers) {
		mgm = list_entry(it, struct aka_av_mgm, list);
		if (str_casematch(&mgm->name, name))
			return mgm;
	}
	return 0;
}

typedef int (*load_aka_av_mgm_f)(struct aka_av_binds *binds);

struct aka_av_mgm *aka_load_mgm(str *name)
{
	char *aka_av_name;
	struct aka_av_mgm *mgm = NULL;
	load_aka_av_mgm_f load_aka_av_mgm;

	aka_av_name = pkg_malloc(sizeof(AKA_AV_MGM_PREFIX) + name->len);
	if (!aka_av_name) {
		LM_ERR("oom for AKA AV name\n");
		return NULL;
	}
	memcpy(aka_av_name, AKA_AV_MGM_PREFIX, sizeof(AKA_AV_MGM_PREFIX) - 1);
	memcpy(aka_av_name + sizeof(AKA_AV_MGM_PREFIX) - 1, name->s, name->len);
	aka_av_name[sizeof(AKA_AV_MGM_PREFIX) - 1 + name->len] = '\0';

	load_aka_av_mgm = (load_aka_av_mgm_f)find_export(aka_av_name, 0);
	if (!load_aka_av_mgm) {
		LM_DBG("could not find binds for AV mgm <%.*s>(%s)\n",
				name->len, name->s, aka_av_name);
		pkg_free(aka_av_name);
		return NULL;
	}
	pkg_free(aka_av_name);
	/* found it - let's create it */
	mgm = pkg_malloc(sizeof *mgm + name->len);
	if (!mgm) {
		LM_ERR("oom for AV mgm\n");
		return NULL;
	}
	memset(mgm, 0, sizeof *mgm);
	mgm->name.s = mgm->buf;
	memcpy(mgm->name.s, name->s, name->len);
	mgm->name.len = name->len;
	if (load_aka_av_mgm(&mgm->binds) < 0) {
		LM_ERR("could not load %.*s AV bindings\n",
				name->len, name->s);
		pkg_free(mgm);
		return NULL;
	}

	return mgm;
}

static struct aka_user_impi *aka_user_impi_new(str *private_id)
{
	struct aka_user_impi *impi = shm_malloc(sizeof *impi + private_id->len);
	if (!impi) {
		LM_ERR("oom for user public identity!\n");
		return NULL;
	}
	impi->impi.s = impi->buf;
	impi->impi.len = private_id->len;
	memcpy(impi->impi.s, private_id->s, private_id->len);
	INIT_LIST_HEAD(&impi->impus);
	return impi;
}

static struct aka_user *aka_user_new(struct aka_user_impi *impi, str *public_id)
{
	struct aka_user *user = shm_malloc(sizeof *user + public_id->len);
	if (!user) {
		LM_ERR("oom for user public identity!\n");
		return NULL;
	}
	memset(user, 0, sizeof *user);
	if (cond_init(&user->cond) != 0) {
		LM_ERR("could not initialize user cond\n");
		shm_free(user);
		return NULL;
	}
	user->impi = impi;
	user->impu.s = user->buf;
	user->impu.len = public_id->len;
	memcpy(user->impu.s, public_id->s, public_id->len);
	INIT_LIST_HEAD(&user->list);
	INIT_LIST_HEAD(&user->avs);
	INIT_LIST_HEAD(&user->async);
	list_add(&user->list, &impi->impus);
	return user;
}

static void aka_user_impi_release(struct aka_user_impi *impi)
{
	if (!list_empty(&impi->impus))
		return;
	/* no more privates pointing to us - remove and release */
	hash_remove_key(aka_users, impi->impi);
	shm_free(impi);
}

static struct aka_user *aka_user_impi_find(struct aka_user_impi *impi, str *public_id)
{
	struct aka_user *user;
	struct list_head *it;

	list_for_each(it, &impi->impus) {
		user = list_entry(it, struct aka_user, list);
		if (str_match(public_id, &user->impu))
			return user;
	}
	return NULL;
}

struct aka_user *aka_user_find(str *public_id, str *private_id)
{
	struct aka_user *user = NULL;
	struct aka_user_impi **impi;
	unsigned int hentry = hash_entry(aka_users, *private_id);

	hash_lock(aka_users, hentry);
	impi = (struct aka_user_impi **)hash_find(aka_users, hentry, *private_id);
	if (impi && *impi) {
		user = aka_user_impi_find(*impi, public_id);
		if (user)
			user->ref++;
	}
	hash_unlock(aka_users, hentry);
	return user;
}

struct aka_user *aka_user_get(str *public_id, str *private_id)
{
	unsigned int hentry;
	struct aka_user_impi **impi;
	struct aka_user *user = NULL;

	hentry = hash_entry(aka_users, *private_id);
	hash_lock(aka_users, hentry);
	impi = (struct aka_user_impi **)hash_get(aka_users, hentry, *private_id);
	if (!impi)
		goto end;
	if (*impi) {
		user = aka_user_impi_find(*impi, public_id);
		if (user)
			goto ref;
	} else {
		*impi = aka_user_impi_new(private_id);
		if (*impi == NULL) {
			LM_ERR("cannot create user private identity!\n");
			goto end;
		}
	}
	user = aka_user_new(*impi, public_id);
	if (!user) {
		LM_ERR("cannot create user privte identity!\n");
		aka_user_impi_release(*impi);
		goto end;
	}
ref:
	user->ref++;
end:
	hash_unlock(aka_users, hentry);
	return user;
}

static void aka_user_try_free(struct aka_user *user)
{
	struct aka_user_impi *impi = user->impi;
	cond_lock(&user->cond);
	if (user->ref != 0 || !list_empty(&user->avs) || !list_empty(&user->async)) {
		cond_unlock(&user->cond);
		return;
	}
	cond_unlock(&user->cond);
	list_del(&user->list);
	cond_destroy(&user->cond);
	shm_free(user);
	/* release pub if not used anymore */
	aka_user_impi_release(impi);
}

void aka_user_release(struct aka_user *user)
{
	unsigned int hentry;
	hentry = hash_entry(aka_users, user->impi->impi);
	hash_lock(aka_users, hentry);
	user->ref--;
	aka_user_try_free(user);
	hash_unlock(aka_users, hentry);
}

static struct aka_av *aka_av_get_state(struct aka_user *user, int algmask, enum aka_av_state state)
{
	struct list_head *it;
	struct aka_av *av = NULL;

	/* find the first free AV */
	list_for_each(it, &user->avs) {
		av = list_entry(it, struct aka_av, list);
		/* check if AV algorithm is suitable */
		if (algmask >= -1 && av->algmask >= 0 && !(algmask & av->algmask)) {
			av = NULL;
			continue;
		}
		if (av->state == state)
			break;
		av = NULL;
	}
	return av;
}

static struct aka_av *aka_av_match(struct aka_user *user, int algmask, str *nonce)
{
	struct list_head *it;
	struct aka_av *av = NULL;

	list_for_each(it, &user->avs) {
		av = list_entry(it, struct aka_av, list);
		if (av->state == AKA_AV_INVALID)
			continue;
		/* check if AV algorithm is suitable */
		if (algmask >= 0 && av->algmask >= 0 && !(algmask & av->algmask))
			continue;
		if (str_match(nonce, &av->authenticate))
			return av;
	}
	return NULL;
}

/* an AV in any state, so that a cached copy is never linked twice */
static struct aka_av *aka_av_match_nonce(struct aka_user *user, str *nonce)
{
	struct list_head *it;
	struct aka_av *av;

	list_for_each(it, &user->avs) {
		av = list_entry(it, struct aka_av, list);
		if (str_match(nonce, &av->authenticate))
			return av;
	}
	return NULL;
}

struct aka_av *aka_av_get_nonce(struct aka_user *user, int algmask, str *nonce)
{
	struct aka_av *av = NULL, *dup;
	int known;

	cond_lock(&user->cond);
	av = aka_av_match(user, algmask, nonce);
	known = (av != NULL);
	if (av) {
		if (av->state != AKA_AV_USING && av->state != AKA_AV_USED)
			av = NULL;
		else
			av->state = AKA_AV_USED;
	}
	cond_unlock(&user->cond);

	if (av || known || !aka_cdb)
		return av;

	/* we have never seen this nonce - another node may have challenged with
	 * it, in which case it published the AV before sending the challenge */
	av = aka_cdb_fetch_av(&user->impu, &user->impi->impi, nonce);
	if (!av)
		return NULL;

	if (av->state != AKA_AV_USING && av->state != AKA_AV_USED) {
		LM_DBG("cached AV for %.*s is in state %d, not challenged yet\n",
				nonce->len, nonce->s, av->state);
		shm_free(av);
		return NULL;
	}

	if (algmask >= 0 && av->algmask >= 0 && !(algmask & av->algmask)) {
		LM_DBG("cached AV for %.*s does not match algorithm mask %d\n",
				nonce->len, nonce->s, algmask);
		shm_free(av);
		return NULL;
	}

	cond_lock(&user->cond);
	/* another process may have adopted the same AV while we were fetching */
	dup = aka_av_match_nonce(user, nonce);
	if (dup) {
		shm_free(av);
		av = dup;
	} else {
		aka_av_insert(user, av);
	}
	av->state = AKA_AV_USED;
	cond_unlock(&user->cond);

	return av;
}

static inline int aka_av_first_bit_mask(int algmask)
{
	int c;
	for (c = 0; c < sizeof(algmask) * 8; c++)
		if (algmask & (1<<c))
			return c;
	return 0;
}

static void aka_av_mark_using(struct aka_av *av, int algmask)
{
	av->state = AKA_AV_USING;
	/*
	 * an algorithm can only be used for one algorithm, so we mark
	 * it as being used only for the first algorithm in the mask
	 */
	av->alg = aka_av_first_bit_mask(algmask);
	av->ts = get_ticks();
}

int aka_av_get_new_wait(struct aka_user *user, int algmask,
		long milliseconds, struct aka_av **av)
{
	int ret = -1;
	struct timespec spec, end, begin;

	cond_lock(&user->cond);
	if (user->error_count) {
		user->error_count--;
		goto end;
	}
	*av = aka_av_get_state(user, algmask, AKA_AV_NEW);
	if (*av == NULL) {
		switch (milliseconds) {
			case 0: /* just peaking */
				break;
			case -1: /* blocking pop */
				do {
					if (user->error_count) {
						user->error_count--;
						goto end;
					}
					cond_wait(&user->cond);
				} while ((*av = aka_av_get_state(user, algmask, AKA_AV_NEW)) == NULL);
				break;
			default:
				do {
					clock_gettime(CLOCK_REALTIME, &begin);
					spec = begin;
					spec.tv_sec += milliseconds / 1000;
					spec.tv_nsec += (milliseconds % 1000) * 1000000;
					errno = 0;
					cond_timedwait(&user->cond, &spec);
					if (user->error_count) {
						user->error_count--;
						goto end;
					}
					*av = aka_av_get_state(user, algmask, AKA_AV_NEW); /* one last time */
					if (cond_has_timedout(&user->cond))
						break;
					if (*av == NULL) {
						/* compute the drift/reminder */
						clock_gettime(CLOCK_REALTIME, &end);
						milliseconds -= (end.tv_sec - begin.tv_sec) * 1000 +
							(end.tv_nsec - begin.tv_nsec) / 1000000;
					}
				} while (*av == NULL && milliseconds > 0);
				break;
		}
	}
	if (*av) {
		aka_av_mark_using(*av, algmask);
		ret = 1;
	} else {
		ret = 0;
	}
end:
	cond_unlock(&user->cond);
	/* published outside the lock: the driver may go to the network */
	if (ret == 1)
		aka_cdb_store_av(&user->impu, &user->impi->impi, *av);
	return ret;
}

int aka_av_get_new(struct aka_user *user, int algmask, struct aka_av **av)
{
	int ret;
	cond_lock(&user->cond);
	if (!user->error_count) {
		ret = 0;
		*av = aka_av_get_state(user, algmask, AKA_AV_NEW);
		if (*av) {
			aka_av_mark_using(*av, algmask);
			ret = 1;
		}
	} else {
		/* account for one error */
		ret = -1;
		user->error_count--;
	}
	cond_unlock(&user->cond);
	/* published outside the lock: the driver may go to the network */
	if (ret == 1)
		aka_cdb_store_av(&user->impu, &user->impi->impi, *av);
	return ret;
}

static struct aka_av *aka_av_new(int algmask, str *authenticate, str *authorize, str *ck, str *ik)
{
	char *p;
	unsigned char *hex, *b64;
	struct aka_av *av = NULL;
	int b64len;

	b64len = calc_base64_encode_len(authenticate->len / 2);
	hex = pkg_malloc((authenticate->len / 2) + b64len);
	if (!hex) {
		LM_ERR("oom for authenticate encoding\n");
		goto end;
	}
	b64 = hex + (authenticate->len / 2);
	if (hex2string(authenticate->s, authenticate->len, (char *)hex) < 0) {
		LM_ERR("could not hexa decode %.*s\n", authenticate->len, authenticate->s);
		goto end;
	}
	base64encode(b64, hex, (authenticate->len / 2));
	av = shm_malloc(sizeof(*av) + b64len + (authorize->len / 2) + ck->len + ik->len);
	if (!av)
		goto end;
	memset(av, 0, sizeof *av);
	av->algmask = algmask;
	p = av->buf;
	av->authenticate.s = p;
	av->authenticate.len = b64len;
	memcpy(p, b64, b64len);
	p += b64len;

	av->authorize.s = p;
	if (hex2string(authorize->s, authorize->len, av->authorize.s) < 0) {
		LM_ERR("could not hexa decode %.*s\n", authorize->len, authorize->s);
		shm_free(av);
		av = NULL;
		goto end;
	}
	av->authorize.len = authorize->len / 2;
	p += av->authorize.len;

	av->ck.s = p;
	av->ck.len = ck->len;
	memcpy(p, ck->s, ck->len);
	p += ck->len;

	av->ik.s = p;
	av->ik.len = ik->len;
	memcpy(p, ik->s, ik->len);
	p += ik->len;
	INIT_LIST_HEAD(&av->list);

end:
	pkg_free(hex);
	return av;
}

void aka_av_free(struct aka_av *av)
{
	list_del(&av->list);
	shm_free(av);
}

static void aka_av_insert(struct aka_user *user, struct aka_av *av)
{
	list_add_tail(&av->list, &user->avs);
}


int aka_av_add(str *pub_id, str *priv_id, int algmask,
		str *authenticate, str *authorize, str *ck, str *ik)
{
	int ret = -1;
	struct aka_av *av;
	struct aka_user *user = aka_user_get(pub_id, priv_id);
	if (!user) {
		LM_INFO("cannot find or create user %.*s/%.*s\n",
				pub_id->len, pub_id->s, priv_id->len, priv_id->s);
		return -1;
	}
	av = aka_av_new(algmask, authenticate, authorize, ck, ik);
	if (!av) {
		LM_ERR("could not create new AV\n");
		goto end;
	}
	cond_lock(&user->cond);
	aka_av_insert(user, av);
	/* we also need to inform users we have an AV */
	if (!list_empty(&user->async))
		aka_signal_async(user, user->async.next);
	cond_signal(&user->cond);
	cond_unlock(&user->cond);
	av->ts = av->new_ts = get_ticks();
	ret = 1;
	LM_DBG("adding av %p\n", av);
end:
	aka_user_release(user);
	return ret;
}

int aka_av_drop_all_user(struct aka_user *user)
{
	int count = 0;
	struct aka_av *av;
	struct list_head *it;

	cond_lock(&user->cond);
	list_for_each(it, &user->avs) {
		av = list_entry(it, struct aka_av, list);
		if (av->state != AKA_AV_INVALID) {
			count++;
			av->state = AKA_AV_INVALID;
		}
		/* a dropped AV must not stay usable on the other nodes; this is an
		 * administrative path, so the driver round trip under the lock is
		 * preferable to copying every nonce out first */
		aka_cdb_remove_av(&user->impu, &user->impi->impi, &av->authenticate);
	}
	cond_unlock(&user->cond);
	return count;
}

int aka_av_drop_all(str *pub_id, str *priv_id)
{
	int count = 0;
	struct aka_user *user = aka_user_find(pub_id, priv_id);

	if (!user) {
		LM_DBG("cannot find user %.*s/%.*s\n",
				pub_id->len, pub_id->s, priv_id->len, priv_id->s);
		return 0;
	}
	count = aka_av_drop_all_user(user);
	aka_user_release(user);
	return count;
}

int aka_av_drop(str *pub_id, str *priv_id, str *nonce)
{
	struct aka_av *av;
	struct aka_user *user = aka_user_find(pub_id, priv_id);

	if (!user) {
		LM_DBG("cannot find user %.*s/%.*s\n",
				pub_id->len, pub_id->s, priv_id->len, priv_id->s);
		return -1;
	}
	cond_lock(&user->cond);
	av = aka_av_match(user, -1, nonce);
	if (av && av->state != AKA_AV_INVALID)
		av->state = AKA_AV_INVALID;
	else
		av = NULL;
	cond_unlock(&user->cond);
	/* a dropped AV must not stay usable on the other nodes */
	aka_cdb_remove_av(pub_id, priv_id, nonce);
	aka_user_release(user);
	return (av?1:0);
}

int aka_av_fail(str *pub_id, str *priv_id, int count)
{
	struct aka_user *user = aka_user_find(pub_id, priv_id);

	if (!user) {
		LM_DBG("cannot find user %.*s/%.*s\n",
				pub_id->len, pub_id->s, priv_id->len, priv_id->s);
		return -1;
	}
	cond_lock(&user->cond);
	user->error_count += count;
	if (!list_empty(&user->async))
		aka_signal_async(user, user->async.next);
	cond_signal(&user->cond);
	cond_unlock(&user->cond);
	aka_user_release(user);
	return 0;
}

void aka_av_set_new(struct aka_user *user, struct aka_av *av)
{
	cond_lock(&user->cond);
	av->state = AKA_AV_NEW;
	av->ts = av->new_ts; /* restore the new timestamp */
	cond_unlock(&user->cond);
	/* back to unchallenged - withdraw it so no node can authorize with it */
	aka_cdb_remove_av(&user->impu, &user->impi->impi, &av->authenticate);
}

void aka_push_async(struct aka_user *user, struct list_head *subs)
{
	cond_lock(&user->cond);
	list_add_tail(subs, &user->async);
	cond_unlock(&user->cond);
}

void aka_pop_unsafe_async(struct aka_user *user, struct list_head *subs)
{
	list_del(subs);
}

void aka_pop_async(struct aka_user *user, struct list_head *subs)
{
	cond_lock(&user->cond);
	aka_pop_unsafe_async(user, subs);
	cond_unlock(&user->cond);
}

static int aka_async_hash_iterator(void *param, str key, void *value)
{
	struct list_head *it, *safe, *uit, *usafe;
	unsigned int ticks = *(unsigned int*)param;
	struct aka_user *user;
	struct aka_user_impi *impi = (struct aka_user_impi *)value;

	list_for_each_safe(uit, usafe, &impi->impus) {
		user = list_entry(uit, struct aka_user, list);
		cond_lock(&user->cond);
		list_for_each_safe(it, safe, &user->async) {
			aka_check_expire_async(ticks, it);
		}
		list_for_each_safe(it, safe, &user->avs) {
			aka_check_expire_av(ticks, list_entry(it, struct aka_av, list));
		}
		cond_unlock(&user->cond);
		aka_user_try_free(user);
	}
	return 0;
}

void aka_async_expire(unsigned int ticks, void* param)
{
	hash_for_each_locked(aka_users, aka_async_hash_iterator, &ticks);
}
