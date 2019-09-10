// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2019 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <script/keyorigin.h>
#include <script/signingprovider.h>
#include <script/standard.h>

#include <key.h>
#include <pubkey.h>
#include <util/system.h>

const SigningProvider &DUMMY_SIGNING_PROVIDER = SigningProvider();

template <typename M, typename K, typename V>
bool LookupHelper(const M &map, const K &key, V &value) {
    auto it = map.find(key);
    if (it != map.end()) {
        value = it->second;
        return true;
    }
    return false;
}

bool HidingSigningProvider::GetCScript(const CScriptID &scriptid,
                                       CScript &script) const {
    return m_provider->GetCScript(scriptid, script);
}

bool HidingSigningProvider::GetPubKey(const CKeyID &keyid,
                                      CPubKey &pubkey) const {
    return m_provider->GetPubKey(keyid, pubkey);
}

bool HidingSigningProvider::GetKey(const CKeyID &keyid, CKey &key) const {
    if (m_hide_secret) {
        return false;
    }
    return m_provider->GetKey(keyid, key);
}

bool HidingSigningProvider::GetKeyOrigin(const CKeyID &keyid,
                                         KeyOriginInfo &info) const {
    if (m_hide_origin) {
        return false;
    }
    return m_provider->GetKeyOrigin(keyid, info);
}

bool FlatSigningProvider::GetCScript(const CScriptID &scriptid,
                                     CScript &script) const {
    return LookupHelper(scripts, scriptid, script);
}
bool FlatSigningProvider::GetPubKey(const CKeyID &keyid,
                                    CPubKey &pubkey) const {
    return LookupHelper(pubkeys, keyid, pubkey);
}
bool FlatSigningProvider::GetKeyOrigin(const CKeyID &keyid,
                                       KeyOriginInfo &info) const {
    std::pair<CPubKey, KeyOriginInfo> out;
    bool ret = LookupHelper(origins, keyid, out);
    if (ret) {
        info = std::move(out.second);
    }
    return ret;
}
bool FlatSigningProvider::GetKey(const CKeyID &keyid, CKey &key) const {
    return LookupHelper(keys, keyid, key);
}

FlatSigningProvider Merge(const FlatSigningProvider &a,
                          const FlatSigningProvider &b) {
    FlatSigningProvider ret;
    ret.scripts = a.scripts;
    ret.scripts.insert(b.scripts.begin(), b.scripts.end());
    ret.pubkeys = a.pubkeys;
    ret.pubkeys.insert(b.pubkeys.begin(), b.pubkeys.end());
    ret.keys = a.keys;
    ret.keys.insert(b.keys.begin(), b.keys.end());
    ret.origins = a.origins;
    ret.origins.insert(b.origins.begin(), b.origins.end());
    return ret;
}

void FillableSigningProvider::ImplicitlyLearnRelatedKeyScripts(
    const CPubKey &pubkey) {
    AssertLockHeld(cs_KeyStore);
    CKeyID key_id = pubkey.GetID();
    // We must actually know about this key already.
    assert(HaveKey(key_id) || mapWatchKeys.count(key_id));
    // This adds the redeemscripts necessary to detect alternative outputs using
    // the same keys. Also note that having superfluous scripts in the keystore
    // never hurts. They're only used to guide recursion in signing and IsMine
    // logic - if a script is present but we can't do anything with it, it has
    // no effect. "Implicitly" refers to fact that scripts are derived
    // automatically from existing keys, and are present in memory, even without
    // being explicitly loaded (e.g. from a file).

    // Right now there are none so do nothing.
}

bool FillableSigningProvider::GetPubKey(const CKeyID &address,
                                        CPubKey &vchPubKeyOut) const {
    CKey key;
    if (!GetKey(address, key)) {
        LOCK(cs_KeyStore);
        WatchKeyMap::const_iterator it = mapWatchKeys.find(address);
        if (it != mapWatchKeys.end()) {
            vchPubKeyOut = it->second;
            return true;
        }
        return false;
    }
    vchPubKeyOut = key.GetPubKey();
    return true;
}

bool FillableSigningProvider::AddKeyPubKey(const CKey &key,
                                           const CPubKey &pubkey) {
    LOCK(cs_KeyStore);
    mapKeys[pubkey.GetID()] = key;
    ImplicitlyLearnRelatedKeyScripts(pubkey);
    return true;
}

bool FillableSigningProvider::HaveKey(const CKeyID &address) const {
    LOCK(cs_KeyStore);
    return mapKeys.count(address) > 0;
}

std::set<CKeyID> FillableSigningProvider::GetKeys() const {
    LOCK(cs_KeyStore);
    std::set<CKeyID> set_address;
    for (const auto &mi : mapKeys) {
        set_address.insert(mi.first);
    }
    return set_address;
}

bool FillableSigningProvider::GetKey(const CKeyID &address,
                                     CKey &keyOut) const {
    LOCK(cs_KeyStore);
    KeyMap::const_iterator mi = mapKeys.find(address);
    if (mi != mapKeys.end()) {
        keyOut = mi->second;
        return true;
    }
    return false;
}

bool FillableSigningProvider::AddCScript(const CScript &redeemScript) {
    if (redeemScript.size() > MAX_SCRIPT_ELEMENT_SIZE) {
        return error(
            "FillableSigningProvider::AddCScript(): redeemScripts > %i bytes "
            "are invalid",
            MAX_SCRIPT_ELEMENT_SIZE);
    }

    LOCK(cs_KeyStore);
    mapScripts[CScriptID(redeemScript)] = redeemScript;
    return true;
}

bool FillableSigningProvider::HaveCScript(const CScriptID &hash) const {
    LOCK(cs_KeyStore);
    return mapScripts.count(hash) > 0;
}

std::set<CScriptID> FillableSigningProvider::GetCScripts() const {
    LOCK(cs_KeyStore);
    std::set<CScriptID> set_script;
    for (const auto &mi : mapScripts) {
        set_script.insert(mi.first);
    }
    return set_script;
}

bool FillableSigningProvider::GetCScript(const CScriptID &hash,
                                         CScript &redeemScriptOut) const {
    LOCK(cs_KeyStore);
    ScriptMap::const_iterator mi = mapScripts.find(hash);
    if (mi != mapScripts.end()) {
        redeemScriptOut = (*mi).second;
        return true;
    }
    return false;
}

static bool ExtractPubKey(const CScript &dest, CPubKey &pubKeyOut) {
    // TODO: Use Solver to extract this?
    CScript::const_iterator pc = dest.begin();
    opcodetype opcode;
    std::vector<uint8_t> vch;
    if (!dest.GetOp(pc, opcode, vch) || !CPubKey::ValidSize(vch)) {
        return false;
    }
    pubKeyOut = CPubKey(vch);
    if (!pubKeyOut.IsFullyValid()) {
        return false;
    }
    if (!dest.GetOp(pc, opcode, vch) || opcode != OP_CHECKSIG ||
        dest.GetOp(pc, opcode, vch)) {
        return false;
    }
    return true;
}

bool FillableSigningProvider::AddWatchOnly(const CScript &dest) {
    LOCK(cs_KeyStore);
    setWatchOnly.insert(dest);
    CPubKey pubKey;
    if (ExtractPubKey(dest, pubKey)) {
        mapWatchKeys[pubKey.GetID()] = pubKey;
        ImplicitlyLearnRelatedKeyScripts(pubKey);
    }
    return true;
}

bool FillableSigningProvider::RemoveWatchOnly(const CScript &dest) {
    LOCK(cs_KeyStore);
    setWatchOnly.erase(dest);
    CPubKey pubKey;
    if (ExtractPubKey(dest, pubKey)) {
        mapWatchKeys.erase(pubKey.GetID());
    }
    // Related CScripts are not removed; having superfluous scripts around is
    // harmless (see comment in ImplicitlyLearnRelatedKeyScripts).
    return true;
}

bool FillableSigningProvider::HaveWatchOnly(const CScript &dest) const {
    LOCK(cs_KeyStore);
    return setWatchOnly.count(dest) > 0;
}

bool FillableSigningProvider::HaveWatchOnly() const {
    LOCK(cs_KeyStore);
    return (!setWatchOnly.empty());
}

CKeyID GetKeyForDestination(const SigningProvider &store,
                            const CTxDestination &dest) {
    // Only supports destinations which map to single public keys, i.e. P2PKH.
    if (auto id = boost::get<PKHash>(&dest)) {
        return CKeyID(*id);
    }
    return CKeyID();
}