/*
 * interface.h
 *
 *  Created on: 2010-4-28
 *      Author: Argon
 */

#ifndef OCGAPI_H_
#define OCGAPI_H_

#include "common.h"

#if defined(OCGCORE_EXPORT_FUNCTIONS)
#ifdef _WIN32
#define OCGCORE_API __declspec(dllexport)
#else
#define OCGCORE_API [[gnu::visibility("default")]]
#endif
#else
#define OCGCORE_API
#endif

struct card_data;

/* ---- query_effect_info -----------------------------------------------------
 * Read-only dump of the duel state that no QUERY_* flag and no MSG_* message
 * reaches: the player-scope registered effects (EFFECT_FLAG_FIELD_ONLY, i.e.
 * the ones with no on-field carrier -- Maxx "C", Droll & Lock Bird, "you
 * cannot X this turn" restrictions), the once-per-turn / once-per-duel /
 * once-per-chain accounting tables, the per-turn activity counters and the
 * "special summon this name once" table.
 *
 * The buffer is a versioned, length-prefixed, section-tagged byte stream; the
 * wire format is specified in the MirrorForce documentation.  A reader that
 * does not know a section id skips it by its length, so sections may be added
 * without a version bump.  Returns the number of bytes written, or LEN_FAIL
 * when buf_size is too small for the dump.
 */
#define EFFECT_INFO_VERSION			1
#define EFFECT_INFO_HEADER_LEN		8
#define EFFECT_INFO_RECORD_LEN		92

#define EFFECT_INFO_SECTION_END			0
#define EFFECT_INFO_SECTION_EFFECTS		1
#define EFFECT_INFO_SECTION_COUNT_CODE	2
#define EFFECT_INFO_SECTION_TURN		3
#define EFFECT_INFO_SECTION_SPSUMMON_ONCE	4
#define EFFECT_INFO_SECTION_ACTIVITY	5
#define EFFECT_INFO_SECTION_PLAYER_STATE	6
#define EFFECT_INFO_SECTION_CARD_STATE	7
#define EFFECT_INFO_SECTION_RELATIONS	8
#define EFFECT_INFO_SECTION_SETCODE	9

/* fixed record widths for the v1 extension sections */
#define EFFECT_INFO_PLAYER_RECORD_LEN	20
#define EFFECT_INFO_CARD_RECORD_LEN	60
#define EFFECT_INFO_RELATION_RECORD_LEN	28
#define EFFECT_INFO_SETCODE_RECORD_LEN	12

/* relation kind in EFFECT_INFO_SECTION_RELATIONS */
#define EFFECT_INFO_REL_MATERIAL		1
#define EFFECT_INFO_REL_GENERIC		2

/* which field::effects container the registered effect lives in */
#define EFFECT_INFO_CONTAINER_AURA			1
#define EFFECT_INFO_CONTAINER_IGNITION		2
#define EFFECT_INFO_CONTAINER_ACTIVATE		3
#define EFFECT_INFO_CONTAINER_TRIGGER_O		4
#define EFFECT_INFO_CONTAINER_TRIGGER_F		5
#define EFFECT_INFO_CONTAINER_QUICK_O		6
#define EFFECT_INFO_CONTAINER_QUICK_F		7
#define EFFECT_INFO_CONTAINER_CONTINUOUS	8
#define EFFECT_INFO_CONTAINER_CARD_SINGLE	9
#define EFFECT_INFO_CONTAINER_CARD_FIELD	10
#define EFFECT_INFO_CONTAINER_CARD_EQUIP	11
#define EFFECT_INFO_CONTAINER_CARD_TARGET	12
#define EFFECT_INFO_CONTAINER_CARD_XMATERIAL	13

/* which of the three count_code tables a record came from */
#define EFFECT_INFO_COUNT_TURN		0
#define EFFECT_INFO_COUNT_DUEL		1
#define EFFECT_INFO_COUNT_CHAIN		2

/* ---- query_duel_state -----------------------------------------------------
 * Viewer-scoped, versioned decision-state snapshot.  Unlike QUERY_* this API
 * applies identity visibility inside the core.  The v2 wire contract lives in
 * the MirrorForce documentation.
 */
#define DUEL_STATE_VERSION		2
#define DUEL_STATE_HEADER_LEN		8

#define DUEL_STATE_VIEW_PLAYER_0	0
#define DUEL_STATE_VIEW_PLAYER_1	1
#define DUEL_STATE_VIEW_SHARED_PUBLIC	2
#define DUEL_STATE_VIEW_OMNISCIENT_LABEL 3

/* Target-only metadata for offline label construction.  These ids are never
 * part of the actor observation and are zero unless explicitly requested. */
#define DUEL_STATE_FLAG_TARGET_ENTITY_IDS 0x1

#define DUEL_STATE_SECTION_END		0
#define DUEL_STATE_SECTION_META		1
#define DUEL_STATE_SECTION_PLAYER	2
#define DUEL_STATE_SECTION_CARD		3
#define DUEL_STATE_SECTION_RELATION	4
#define DUEL_STATE_SECTION_EFFECT	5
#define DUEL_STATE_SECTION_EFFECT_OBJECT 6

#define DUEL_STATE_META_RECORD_LEN	64
#define DUEL_STATE_PLAYER_RECORD_LEN	64
#define DUEL_STATE_CARD_RECORD_LEN	132
#define DUEL_STATE_RELATION_RECORD_LEN	32
#define DUEL_STATE_EFFECT_RECORD_LEN	132
#define DUEL_STATE_EFFECT_OBJECT_RECORD_LEN 16

#define DUEL_STATE_CARD_IDENTITY_VISIBLE 0x1
#define DUEL_STATE_CARD_SEQUENCE_VISIBLE 0x2
#define DUEL_STATE_CARD_ANONYMOUS	0x4
#define DUEL_STATE_CARD_OVERLAY		0x8
#define DUEL_STATE_CARD_OWN_KNOWLEDGE	0x10

#define DUEL_STATE_REL_EQUIP		1
#define DUEL_STATE_REL_OVERLAY		2
#define DUEL_STATE_REL_SUMMON_MATERIAL	3
#define DUEL_STATE_REL_EFFECT_TARGET	4
#define DUEL_STATE_REL_GENERIC		5
#define DUEL_STATE_REL_REASON_CARD	6
#define DUEL_STATE_REL_ATTACK_ANNOUNCED	7
#define DUEL_STATE_REL_ATTACKED		8
#define DUEL_STATE_REL_BATTLED		9

typedef byte* (*script_reader)(const char* script_name, int* len);
typedef uint32_t (*card_reader)(uint32_t code, card_data* data);
typedef uint32_t (*message_handler)(intptr_t pduel, uint32_t msg_type);

byte* read_script(const char* script_name, int* len);
uint32_t read_card(uint32_t code, card_data* data);
uint32_t handle_message(void* pduel, uint32_t message_type);

#ifdef __cplusplus
extern "C" {
#endif

OCGCORE_API void set_script_reader(script_reader f);
OCGCORE_API void set_card_reader(card_reader f);
OCGCORE_API void set_message_handler(message_handler f);

OCGCORE_API intptr_t create_duel(uint_fast32_t seed);
OCGCORE_API intptr_t create_duel_v2(uint32_t seed_sequence[]);
/* MirrorForce information-set search: replace only the duel's *future* PRNG
 * stream after a hidden-state particle has been realized.  Exactly
 * SEED_COUNT words are consumed.  This does not shuffle or otherwise mutate
 * any current zone; callers must snapshot/rollback around hypothetical lines. */
OCGCORE_API int32_t duel_set_future_seed(intptr_t pduel, const uint32_t seed_sequence[]);
/* Experimental client shadow: hydrate an uninitialized hidden placeholder in
 * place. Only reserved 999000001/2/3/4 placeholders are accepted; real identities
 * cannot be rewritten. 999000001 -> 999000003 (999000004 for the field zone)
 * only creates an sset-capable execution proxy, without a script. It does NOT assert printed spell type
 * (some monsters can also be set in SZONE); a monster hydrated in SZONE whose EFFECT_MONSTER_SSET lets it be set
 * there gets the type change field::sset gives it. No adjust/event is run.
 * All arena changes, including
 * Lua globals and registered effects, roll back if initialization fails.
 * 0 success; -1 invalid duel/boundary; -2 non-placeholder/public card;
 * -3 invalid identity/pool; -4 snapshot failure; -5 script failure;
 * -6 rollback failure (caller must permanently discard this shadow).
 * Call only from an idle host C boundary, never from a Lua/C callback. */
OCGCORE_API int32_t duel_hydrate_card(intptr_t pduel, uint8_t playerid,
    uint8_t location, uint8_t sequence, uint32_t expected_placeholder, uint32_t code);
/* The inverse of duel_hydrate_card for a hidden card whose identity the public
 * stream no longer pins to this object (after a hidden move or shuffle): the
 * card's own initial effects are removed (as card::replace_effect removes them), and it becomes the given
 * placeholder again -- 999000001 for a main-deck card in the hand, the deck, a
 * face-down monster or banished zone, 999000002 for an extra-deck monster,
 * 999000003 for a face-down spell/trap zone card, 999000004 for the field zone.
 * The card object, its status and every external effect, flag and relation
 * stay. Refused (-2) for a code other than expected_code, a placeholder of
 * another pool, a public (face-up, off hand and deck) card, a card with
 * counters or an active unique-on-field rule, and a card one of whose effects
 * a pending choice, chain link or trigger still holds. A set proxy (999000003,
 * 999000004) back in the deck or the hand is blanked too, to 999000001: it
 * stood only for a card set face down. The same rollback contract and return
 * codes as duel_hydrate_card otherwise. */
OCGCORE_API int32_t duel_blank_card(intptr_t pduel, uint8_t playerid,
    uint8_t location, uint8_t sequence, uint32_t expected_code, uint32_t placeholder);
/* Belief-world search, dormant identities: create, before any deck card exists and once per duel, one card object
 * per code (strictly ascending) at location 0 with no controller, running its initial_effect, so that every
 * duel-level registration a card's script makes when it loads -- guarded global watchers, activity counters, global
 * flags -- is made once, at duel creation, whichever cards the decks hold. report[i] tells what code i's load did at
 * duel level: 1 registered event watchers (continuous effects on events), 2 added activity counters, 4 enabled
 * global flags, 8 registered other duel-level effects (rules), 16 gave an effect to another card, 32 registered an
 * effect it does not own (Effect.GlobalEffect). Afterwards a card outside these codes that registers at duel level
 * while it initializes raises a script error.
 * 0 success; -1 invalid duel or not at an idle host boundary; -2 called after a deck card or twice; -3 codes not
 * strictly ascending, unknown, or a follower placeholder. */
OCGCORE_API int32_t duel_create_dormant(intptr_t pduel, const uint32_t codes[], int32_t count, uint32_t report[]);
/* The load-side-effect scan of a dormant table's generator: one more inert object of ``code`` in a scratch duel (no
 * dormant identities, before the first turn), with duel_create_dormant's report bits for its load. Probing a code
 * twice tells per-instance registrations (the second load registers again) from guarded ones. 0 success; -1 invalid
 * duel or state; -3 unknown code or a follower placeholder. */
OCGCORE_API int32_t duel_probe_load(intptr_t pduel, uint32_t code, uint32_t* report);
/* Belief-world search: give one player's hidden places other identities in place. Every hand and main deck card
 * (in sequence order), each named face-down card (location, sequence, code triples: monster or spell/trap zone,
 * face-down banished) and, with n_extra >= 0, every face-down extra deck card takes the code given for it: the card
 * object keeps its place, position, history, field ids and status; its identity's own effects (initial effects and
 * its own flag effects), its duel-level registrations and its script-set fields leave, and the new identity's
 * initial_effect runs there (a monster set in the spell/trap zone keeps its set type, as duel_hydrate_card does).
 * Every named place is rewritten, also one whose code does not change, so what a replacement does depends on the
 * assignment only, never on the identities it replaces. Needs dormant identities (duel_create_dormant).
 * 0 success; -1 invalid duel, no dormant identities, not at an idle host boundary, or malformed counts; -2 a count
 * that does not match a zone, an empty or face-up face-down place, or a place named twice; -3 an unknown code, a
 * follower placeholder, or an extra deck card for a main place (or the reverse); -4 a place whose card carries more
 * than its own identity (another card's effect, a non-flag effect of its own beyond its initial ones, counters,
 * relations, battle records, targets, equipment, materials, a turn counter, an operation in progress) or that a
 * pending chain link, trigger or operation holds (the caller keeps every place its prompt's menu names); -5 snapshot failure; -6 a script failed or a
 * message would reach the players: rolled back, the duel unchanged; -7 rollback failure (discard the duel). Every
 * refusal leaves the duel byte-identical. */
OCGCORE_API int32_t duel_replace_hidden(intptr_t pduel, uint8_t playerid, const uint32_t hand[], int32_t n_hand,
    const uint32_t deck[], int32_t n_deck, const uint32_t facedown[], int32_t n_facedown, const uint32_t extra[],
    int32_t n_extra);
/* Why each hidden card of a player could not change identity in place (duel_replace_hidden's -4), 0 when it could:
 * the hand and the main deck in sequence order, the face-down monster zone, spell/trap zone and banished cards in
 * zone order, then the face-down extra deck. Bits: 1 counters, 2 relations, 4 battle records, 8 equipment, targets
 * or materials, 16 another card's effect, 32 an own effect beyond its initial ones and flags, 64 an operation in
 * progress, 128 a unique-on-field rule, 256 held by a pending chain link or trigger, 1024 in a pending operation's
 * group, 2048 a turn counter or an assumed property, 4096 indestructibility counts or copied effects (512 is
 * unused: a live selection's cards are its prompt's menu, which the caller keeps). Returns the count, -1 invalid
 * duel, -3 cap too small. */
OCGCORE_API int32_t duel_hidden_blockers(intptr_t pduel, uint8_t playerid, uint32_t out[], int32_t cap);
/* A full collection of the duel's Lua heap at an idle host boundary (stress lanes: replacement and rollback under
 * collection pressure; finalizers mark script groups collected, deleted at the next deterministic point as usual).
 * 0 success, -1 invalid duel or not at an idle host boundary. */
OCGCORE_API int32_t duel_collect_garbage(intptr_t pduel);
/* The Xyz materials of a player's monsters with their owners (public: the stream shows where each material came
 * from), read-only: per material four words -- the host's sequence, the material's index, its code and its owner --
 * in zone and material order. Returns the material count; -1 invalid duel; -3 cap (words) too small. */
OCGCORE_API int32_t query_overlay_owners(intptr_t pduel, uint8_t playerid, uint32_t out[], int32_t cap);
/* Duel.SelectTarget calls of the duel so far that found fewer candidates than their minimum (the selection then
 * returns no card), and in *code the handler code of the last one's chain link. Read-only. Returns the count; -1
 * invalid duel. */
OCGCORE_API int32_t query_target_shortfall(intptr_t pduel, uint32_t* code);
/* The hand limit the End Phase applies to a player (processor.cpp, step 20 of the phase: the last EFFECT_HAND_LIMIT
 * affecting the player, else 6). Returns the limit; -1 invalid duel or player. */
OCGCORE_API int32_t query_hand_limit(intptr_t pduel, uint8_t playerid);
/* The most duels a process can hold alive at once: create_duel / create_duel_v2 return 0 beyond it. */
OCGCORE_API int32_t duel_live_capacity();
/* The effect a response to the waiting prompt would activate, read-only: option ``index`` of the activatable effects
 * of a waiting MSG_SELECT_IDLECMD, MSG_SELECT_BATTLECMD or MSG_SELECT_CHAIN (message order), or, for a waiting
 * MSG_SELECT_EFFECTYN that asks whether to activate a single trigger effect, that effect (index 0). Returns flags --
 * 1 an activation, 2 the effect has EFFECT_FLAG_CARD_TARGET, 4 a cost function, 8 a target function, 16 one of
 * its functions called Effect.IsCostChecked so far (a cost paid in its target function) -- with out[0]
 * the handler's code and out[1] the effect's description (what the prompt message listed); 0 when the waiting prompt
 * activates nothing at ``index``; -1 invalid duel. */
OCGCORE_API int32_t query_activation_flags(intptr_t pduel, int32_t index, uint32_t out[2]);
/* A 64-bit FNV-1a digest of the duel's whole arena (its every allocation, Lua heap included): equal digests, equal
 * duels. 0 success, -1 invalid duel. */
OCGCORE_API int32_t duel_arena_digest(intptr_t pduel, uint64_t* digest);
/* Local tactical uncertainty-read instrumentation, version 1. Dormant by
 * default, never changes a rule result and exposes no identities or RNG words.
 * begin accepts <=512 distinct live local UIDs, sorted internally. Invalid
 * input or an already enabled audit leaves all state unchanged. end disables
 * recording but retains counters. All data rolls back with native snapshots.
 * state writes exactly 8 uint64 words: version, enabled, UID count, then
 * Lua-card, Lua-group, Lua-effect, card-getter/query, random-call counters.
 * Counters saturate. Coverage is deliberately explicit: callers still need
 * their public root/asset and future-message checks; zero alone is no generic
 * hidden-state noninterference certificate. Idle serialized C boundary only.
 * Returns 0 success, -1 invalid/nonidle duel, -2 bad input/buffer, -3 active
 * begin or inactive end, -4 a UID is not live in this duel. Reads never mutate.
 */
OCGCORE_API int32_t duel_tactical_audit_begin(intptr_t pduel, const uint64_t* uids, int32_t count);
OCGCORE_API int32_t duel_tactical_audit_state(intptr_t pduel, uint64_t* out, int32_t capacity);
OCGCORE_API int32_t duel_tactical_audit_end(intptr_t pduel);
/* v2 additionally observes the specified player's unknown deck order.
 * It does not reveal that order. begin_v2 has the same atomicity/UID rules as
 * begin, and order_player must be 0 or 1. state_v2 writes 12 uint64 words:
 * [2, enabled, unknown_UID_count, order_player+1 (0 when not configured),
 *  the five v1 counters, order_reads, own_deck_shuffles, other_random].
 * The v1 ABI remains unchanged. Covered order reads include Lua current and
 * previous card sequence, deck-top access/confirmation/sorting, native draws,
 * mills and deck-discard cost checks. A full-deck set query is not a positional
 * read; callers must still certify their script/filter support domain.
 * A zero counter is not an unrestricted hidden-state independence proof.
 */
OCGCORE_API int32_t duel_tactical_audit_begin_v2(intptr_t pduel, const uint64_t* uids, int32_t count, int32_t order_player);
OCGCORE_API int32_t duel_tactical_audit_state_v2(intptr_t pduel, uint64_t* out, int32_t capacity);
/* Closed hypothetical shuffle plan, only before the first process() and only
 * once per duel (also after finish). Idle C boundary only. No host UID input.
 * words: [version=1, event_count], then each [message, player, location, n,
 * sequences[n], destination_from_source[n], expected_source_codes[n]].
 * Sequences are strictly increasing; ordinary zones use 0..n-1. The mapping
 * is a full permutation, not card codes used as copy identifiers. Limits:
 * 4096 events, 262144 total participants, n<=255 (field n=2..5, seq<=4).
 * An extra shuffle may have zero face-down participants; face-up extras stay.
 * Install validates every word before any change, copies all storage into the
 * duel arena, and refuses existing Debug.ForceShuffle. That Lua helper also
 * refuses every later call on an installed/finished plan.
 * Every actually emitted shuffle consumes one entry in order. Original random
 * draws happen unchanged; deterministic pseudo-shuffles and set-group results
 * are only checked, never overwritten. Wrong/missing/extra events are sticky
 * faults, and process() returns PROCESSOR_REPLAY_ERROR; ignore its partial
 * messages. finish requires exact consumption, then future shuffles are natural.
 * Return 0 success; -1 invalid/nonidle duel, -2 malformed input, -3 wrong state,
 * -4 failed/unconsumed plan, -5 mixed legacy force, -6 allocation failed (rolled
 * back), -7 allocation failure with rollback failure. Reads never allocate or
 * advance. state needs 9 words: version,mode,total,consumed,fault,last message,
 * last player,last location,last participant count. receipts returns required
 * uint64 words for (out=NULL,cap=0), otherwise words written; -2 small capacity
 * leaves out untouched. Receipt words begin with the same 9-word header; each
 * consumed record is [ordinal,flavor,message,player,location,n, sequences[n],
 * before_uids[n],after_uids[n]]. flavor 1 random zone,2 deterministic zone,
 * 3 random field,4 deterministic field. Queue/backing/cursor/fault/receipts
 * all snapshot and roll back together. No observation or true-deal interface. */
OCGCORE_API int32_t duel_shuffle_plan_install(intptr_t pduel, const uint32_t* words, int32_t nwords);
OCGCORE_API int32_t duel_shuffle_plan_state(intptr_t pduel, uint32_t* out, int32_t cap);
OCGCORE_API int32_t duel_shuffle_plan_receipts(intptr_t pduel, uint64_t* out, int32_t cap);
OCGCORE_API int32_t duel_shuffle_plan_finish(intptr_t pduel);
/* Random outcomes a following client read from the public stream, for its own
 * local duel only. duel_force_random_outcomes appends coin/dice results that
 * the next tosses take in order; duel_force_random_select appends one
 * selection that the next Group.RandomSelect takes: each card by its code when
 * codes[i] is nonzero (copies are not told apart), else by its location
 * (controller | location << 8 | sequence << 16); codes may be NULL. The natural random words are drawn all
 * the same. An entry that does not fit (a value out of range, a card not in the
 * group, another count) is dropped and counted as a miss.
 * duel_forced_random_state writes pending outcomes, pending selections and
 * misses; duel_clear_forced_random drops all three. Idle host boundary only;
 * the queues are duel state and roll back with snapshots. */
OCGCORE_API int32_t duel_force_random_outcomes(intptr_t pduel, int32_t count, const uint8_t* values);
OCGCORE_API int32_t duel_force_random_select(intptr_t pduel, int32_t count, const uint32_t* locations,
    const uint32_t* codes);
OCGCORE_API int32_t duel_forced_random_state(intptr_t pduel, int32_t* state);
OCGCORE_API int32_t duel_clear_forced_random(intptr_t pduel);
/* A following client's forced activation (duel::forced_activations): the
 * activatable effects with this description of cards with this code the
 * player controls pass their script checks until one of them is chained, and
 * while that link is solved blank placeholders in the player's hidden zones
 * pass the script's card filters; the same activation already pending is one
 * mark. Returns 0, -1 for an invalid duel or player, -3 when the capacity is
 * exceeded. duel_forced_activation_state
 * writes the pending and linked counts; duel_clear_forced_activations drops
 * the pending ones and returns how many there were. Idle host boundary only;
 * the lists are duel state and roll back with snapshots. */
OCGCORE_API int32_t duel_force_activation(intptr_t pduel, uint8_t playerid, uint32_t code, uint32_t description);
OCGCORE_API int32_t duel_forced_activation_state(intptr_t pduel, int32_t* state);
OCGCORE_API int32_t duel_clear_forced_activations(intptr_t pduel);
/* Local blank-client bookkeeping only; never actor/label observations.
 * Version 1 is little-endian, header <4sHHII>: "LEM1", version=1,
 * record_width=32, count, total_bytes. Rows sorted by cardid are <QQBBBBIII>:
 * uid, overlay_parent_uid, owner, RAW controller, RAW location,
 * placeholder (0=not a reserved blank, 1=main, 2=extra, 3=sset proxy),
 * sequence, overlay_ordinal, reserved=0. No identity/effect/RNG/Lua data.
 * Location 0 includes unplaced/temp objects: sequence=UINT32_MAX; no parent.
 * Overlay uses location=0x80, raw controller (normally PLAYER_NONE), and
 * sequence=ordinal in its parent's materials, not the parent's field slot.
 * (buf=null, size=0) returns required bytes. All errors leave buf untouched:
 * -1 invalid duel/non-idle C boundary, -2 unknown ABI, -3 buffer, -4 metadata.
 * Pure read: no allocation, Lua, adjust, query caches or arena writes.
 * UID lifetime is this duel only; rollback may reuse IDs for future births.
 * C callers own serialization and provenance; the Python client API restricts
 * access to the registered controller/root/branch, not arbitrary duels. */
#define LOCAL_ENTITY_MAP_VERSION 1
#define LOCAL_ENTITY_MAP_HEADER_LEN 16
#define LOCAL_ENTITY_MAP_RECORD_LEN 32
OCGCORE_API int32_t query_local_entity_map(intptr_t pduel, uint32_t version,
    byte* buf, int32_t buf_size);
/* Local empty optional phase windows. Header 32 bytes, record 12 bytes.
 * Enable only before the first process/turn; default off. Idle-only query.
 * A LOCAL witness, never a server private menu or hidden card identity. */
/* Client search branches and followers: put one player's main deck or hand
 * into an exact object order.  uids[] names every card of that zone exactly
 * once by card::cardid (the local entity map UID) in slot order: uids[0]
 * becomes slot 0 (the deck bottom) and the last entry the last slot (the deck
 * top).  Only the zone vector's pointer order and each card's current.sequence
 * change: no object is created, destroyed or re-bound, identities, positions,
 * flags, relations, counters and history stay with their objects, field
 * effects are not cancelled or re-added (the zone is unchanged), and no Lua,
 * event or random word runs.  The whole order is checked before anything is
 * written.
 * 0 success; -1 invalid duel/player/zone or not at an idle host C boundary;
 * -2 count differs from the zone size; -3 a UID is missing, repeated or not in
 * this zone; -4 the deck is reversed or the zone holds a face-up (public) card. */
OCGCORE_API int32_t duel_reorder_zone_uids(intptr_t pduel, uint8_t playerid, uint8_t location,
    const uint64_t uids[], int32_t count);
/* Set the order in which the core processes these cards when they meet in one
 * group (every card_set sorts by card::sortid): uids[0] first.  The listed
 * cards take the sort positions they already hold among themselves, in the
 * listed order; no other card's position changes.  A following client uses
 * it to process a group operation in the order the server's packets show; it
 * changes no identity, place or history, and runs no Lua, event or random
 * word.  Every card_set of the duel is sorted again: all groups (a group
 * iterated with GetFirst/GetNext keeps its current card), each card's sets
 * and the processor's sets.
 * A pending group set (PROCESSOR_SSET_G) keeps the cards it has still to place
 * in its own set, which is sorted again too; while any other unit that keeps
 * its own card set is pending (draw, control, trap monster adjustment, summon
 * rule, monster set, position change) nothing is changed.
 * 0 success; -1 invalid duel or not at an idle host C boundary; -2 fewer than
 * two UIDs; -3 a UID is missing or repeated; -4 a pending unit keeps its own
 * card set; -5 a card_set is out of order after the resort; -6 a pending group
 * set would change the order of a card it already took, or of the card whose
 * zone was just answered (the order belongs before the operation began). */
OCGCORE_API int32_t duel_order_cards(intptr_t pduel, const uint64_t uids[], int32_t count);
/* The card groups the pending processor units operate on (each unit's
 * target when it is a live group, then a pending group set's cards still to
 * place), innermost first, read-only: u32 group
 * count, then per group u32 card count and per card u64 cardid (the entity
 * UID) and u32 code, in the group's order.  With a null buffer (and size 0)
 * the byte count; -1 invalid duel; -3 buffer too small. */
OCGCORE_API int32_t query_operation_groups(intptr_t pduel, byte* buf, int32_t buf_size);
/* duel_reorder_zone_uids for LOCATION_DECK. */
OCGCORE_API int32_t duel_reorder_deck_uids(intptr_t pduel, uint8_t playerid,
    const uint64_t uids[], int32_t count);
OCGCORE_API int32_t duel_set_phase_pass_audit(intptr_t pduel, uint32_t version, uint8_t enabled);
OCGCORE_API int32_t query_phase_pass_audit(intptr_t pduel, uint32_t version, byte* buf, int32_t buf_size);
OCGCORE_API void start_duel(intptr_t pduel, uint32_t options);
OCGCORE_API void end_duel(intptr_t pduel);
OCGCORE_API void set_player_info(intptr_t pduel, int32_t playerid, int32_t lp, int32_t startcount, int32_t drawcount);
OCGCORE_API void get_log_message(intptr_t pduel, char* buf);
OCGCORE_API int32_t get_message(intptr_t pduel, byte* buf);
OCGCORE_API uint32_t process(intptr_t pduel);
OCGCORE_API void new_card(intptr_t pduel, uint32_t code, uint8_t owner, uint8_t playerid, uint8_t location, uint8_t sequence, uint8_t position);
OCGCORE_API void new_tag_card(intptr_t pduel, uint32_t code, uint8_t owner, uint8_t location);
OCGCORE_API int32_t query_card(intptr_t pduel, uint8_t playerid, uint8_t location, uint8_t sequence, uint32_t query_flag, byte* buf, int32_t use_cache);
OCGCORE_API int32_t query_field_count(intptr_t pduel, uint8_t playerid, uint8_t location);
OCGCORE_API int32_t query_field_card(intptr_t pduel, uint8_t playerid, uint8_t location, uint32_t query_flag, byte* buf, int32_t use_cache);
OCGCORE_API int32_t query_field_info(intptr_t pduel, byte* buf);
OCGCORE_API int32_t query_effect_info(intptr_t pduel, byte* buf, int32_t buf_size);
OCGCORE_API int32_t query_duel_state(intptr_t pduel, uint8_t view, uint32_t flags, byte* buf, int32_t buf_size);
OCGCORE_API void set_responsei(intptr_t pduel, int32_t value);
OCGCORE_API void set_responseb(intptr_t pduel, byte* buf);
OCGCORE_API int32_t preload_script(intptr_t pduel, const char* script_name);
OCGCORE_API byte* default_script_reader(const char* script_name, int* len);

#ifdef __cplusplus
} /* end of the 'extern "C"' block */
#endif

#endif /* OCGAPI_H_ */
