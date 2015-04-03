#ifndef TARANTOOL_BOX_SOPHIA_ENGINE_H_INCLUDED
#define TARANTOOL_BOX_SOPHIA_ENGINE_H_INCLUDED
/*
 * Redistribution and use in source and binary forms, with or
 * without modification, are permitted provided that the following
 * conditions are met:
 *
 * 1. Redistributions of source code must retain the above
 *    copyright notice, this list of conditions and the
 *    following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above
 *    copyright notice, this list of conditions and the following
 *    disclaimer in the documentation and/or other materials
 *    provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY <COPYRIGHT HOLDER> ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
 * <COPYRIGHT HOLDER> OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */
#include "engine.h"

struct SophiaEngine: public Engine {
	SophiaEngine();
	virtual void init();
	virtual Handler *open();
	virtual Index *createIndex(struct key_def*);
	virtual void dropIndex(Index*);
	virtual void keydefCheck(struct space *space, struct key_def*f);
	virtual void begin(struct txn*, struct space*);
	virtual void commit(struct txn*);
	virtual void rollback(struct txn*);
	virtual void begin_recover_snapshot(int64_t);
	virtual void end_recover_snapshot();
	virtual void begin_join();
	virtual void end_recovery();
	virtual void join(Relay*);
	virtual int begin_checkpoint(int64_t);
	virtual int wait_checkpoint();
	virtual void commit_checkpoint();
	virtual void abort_checkpoint();
	void *env;
private:
	int64_t m_prev_checkpoint_lsn;
	int64_t m_checkpoint_lsn;
	int recovery_complete;
};

void sophia_info(void (*)(const char*, const char*, void*), void*);
void sophia_raise(void*);

extern "C" {
int sophia_schedule(void);
}

#endif /* TARANTOOL_BOX_SOPHIA_ENGINE_H_INCLUDED */
