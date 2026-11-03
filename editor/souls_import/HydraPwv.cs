// SPDX-License-Identifier: GPL-3.0-or-later
using SoulsFormats;
using System.Security.Cryptography;

// Rebuild the typed native arrays. DS1's 4/8/8 records cannot be forwarded as
// BB's 8/8/16 records. Ragdoll parameter IDs must be explicitly owned by caller.
internal static class HydraPwv {
    internal sealed record Head(int Slot,char Branch,int Group,int CollisionMask,int SourceRagdollParam,int OwnedRagdollParam,IReadOnlyList<int> BodyIndices);
    internal sealed record Report(string SourceSHA256,string NativeSHA256,int BodyRecords,int AnimationRecords,int RagdollRecords,int PreservedNativeAnimationRecords,
        IReadOnlyList<int> RequiredSourceRagdollIDs,IReadOnlyList<Head> Heads,IReadOnlyList<string> Limitations);
    internal sealed record Result(byte[] Pwv,Report Report);
    internal const string NativePwvSHA256="a46ea5d63653f1aa003332efb0e76f320f3a34a9f45e710fbfd5cc68d1bb9929";
    internal static Result Convert(byte[] nativePwv,HydraPhysicsSource source,NativeRigAppend.Plan plan,IReadOnlyDictionary<int,int> ragdollIds) {
        HydraPhysicsRig.ValidatePlan(source,plan);string nativeHash=System.Convert.ToHexStringLower(SHA256.HashData(nativePwv));
        HydraPhysicsPacking.Need(nativeHash==NativePwvSHA256,"unverified c2730 PWV carrier");
        var native=HKXPWV.Read(nativePwv);
        HydraPhysicsPacking.Need(native.BoneMapEntries.Count==60&&native.AnimBoneEntries.Count==110&&native.RagdollBoneEntries.Count==60&&native.Write().SequenceEqual(nativePwv),"native PWV typed round-trip contract");
        var required=source.RagdollRecords.Where(r=>r.UnknownA>=0).Select(r=>(int)r.UnknownA).Distinct().Order().ToArray();
        HydraPhysicsPacking.Need(required.SequenceEqual(Enumerable.Range(1400,7))&&required.All(id=>ragdollIds.TryGetValue(id,out int owned)&&owned>0&&owned!=id)&&
            required.Select(id=>ragdollIds[id]).Distinct().Count()==7,"missing/conflicting owned Hydra RagdollParam identities");
        var originalNativeIds=native.RagdollBoneEntries.Select(r=>r.RagdollParamID).Where(id=>id>=0).ToHashSet();
        HydraPhysicsPacking.Need(required.All(id=>!originalNativeIds.Contains(ragdollIds[id])),"owned RagdollParam overlaps carrier references");
        var prefix=native.AnimBoneEntries.ToList();
        native.BoneMapEntries=source.BodyFlags.Select(value=>new HKXPWV.BoneMapEntry {
            UnknownValueA=unchecked((short)(value&65535)),UnknownValueB=unchecked((short)(value>>16))}).ToList();
        native.AnimBoneEntries=prefix.Concat(source.AnimationRecords.Select(r=> {
            HydraPhysicsPacking.Need(r==new HydraPhysicsSource.PartRecord(-1,0,0),"unsupported source animation PWV entry");return new HKXPWV.AnimBoneEntry();
        })).ToList();
        native.RagdollBoneEntries=source.RagdollRecords.Select(r=>new HKXPWV.RagdollBoneEntry {
            RagdollParamID=r.UnknownA<0?-1:ragdollIds[r.UnknownA],DamageAnimID=r.UnknownB,NPCPartGroupIndex=checked((byte)r.Group),Unknown=0,UnknownBB=-1}).ToList();
        var heads=source.Parts.Select(part=> {
            var ids=part.BodyIndices.Select(i=>(int)source.RagdollRecords[i].UnknownA).Distinct().ToArray();
            HydraPhysicsPacking.Need(ids.Length==1&&ids[0]==1400+part.Slot,"source head parameter ownership changed");
            return new Head(part.Slot,part.Branch,part.Group,part.CollisionMask,ids[0],ragdollIds[ids[0]],part.BodyIndices);
        }).ToArray();
        var output=native.Write();HydraPhysicsPacking.Need(output.Length==5208,"native Hydra PWV stride/population mismatch");
        return new(output,new(source.PwvSHA256,nativeHash,97,356,97,110,required,heads,[
            "Source RagdollParamID is sign-extended/remapped; DamageAnimID and NPCPartGroupIndex are preserved. Native-only UnknownBB uses the actual carrier default -1.",
            "Caller must install all seven owned 48-byte source RagdollParam rows (partsHitMaskNo1..7), create matching part groups and sever events, and validate native contact/severing behavior.",
            "The first 110 animation entries preserve the original native SkeletonParam refs; all 246 source animation entries are the source's default -1/0."]));
    }
}
