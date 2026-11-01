// SPDX-License-Identifier: GPL-3.0-or-later
using SoulsFormats;
using System.Numerics;

// A source-visible rig owns the body, hands and equipment. The selected native
// controller owns combat decisions and physical body tracks. They share action
// playback, with source hit/shot windows normalized onto the native clip time.
internal static class HollowCombatCharacters {
    internal sealed record Report(string SourceModel,string Role,string NativeModel,int NativeBones,int VisibleBones,
        int MotionClips,int ReferenceFallbacks,int MeleeActions,int ProjectileActions,int? SourceProjectileDummy,
        IReadOnlyList<HollowCombatMotion.EventBinding> CombatActions,IReadOnlyList<string> Limitations) {
        public HollowDeathRig.Report? DeathPhysics {get;init;}
        public IReadOnlyList<HollowDeathMotion.Binding> OwnedDeathClips {get;init;}=[];
    }
    internal sealed record Result(SkinBake Skin,Report Report);
    static string Leaf(string path)=>path.Replace('\\','/').Split('/').Last();
    internal static Result Package(Func<string,byte[]?> readSource,Func<string,byte[]> readNative,
        HollowCombatProfiles.Profile profile,string target,byte[] sourceModel,List<TPF.Texture> textures,
        IReadOnlySet<int> masks,SourceClothPose.Result? cloth,Dictionary<string,byte[]> files) {
        const string template=HollowCombatProfiles.NativeModel;
        var character=BND4.Read(readNative("chr/"+template+".chrbnd.dcx"));
        var animation=BND4.Read(readNative("chr/"+template+".anibnd.dcx"));
        var visual=character.Files.Single(f=>Leaf(f.Name).EndsWith(".flver",StringComparison.OrdinalIgnoreCase));
        var skeleton=animation.Files.Single(f=>f.ID==1000000);
        NativeRigAppend.ValidateCharacter(animation.Files.Single(f=>f.ID==7000400).Bytes.ToArray());
        var motion=HollowCombatMotion.Create(readSource,profile.SourceModel,profile.Role,animation);
        var reference=SourceAnimation.Load(readSource,profile.SourceModel,0);
        var prepared=HollowSourceSkin.PrepareModel(sourceModel,cloth);
        int[] muzzleIDs=profile.SourceProjectileDummy<0?[]:[profile.SourceProjectileDummy];
        var plan=HollowSourceSkin.Rig(prepared,visual.Bytes.ToArray(),skeleton.Bytes.ToArray(),reference,
            rigidAnchors:cloth?.Anchors,masks:masks,sourceMuzzleIDs:muzzleIDs);
        if(plan.Original.Names.Length!=HollowCombatProfiles.NativeBones)
            throw new InvalidDataException("Native hollow controller rig changed");
        var render=FLVER2.Read(plan.Model);
        // Retain the established opaque/alpha-test source character shaders.
        var shaders=BND4.Read(readNative("chr/c2610.chrbnd.dcx"));
        var shaderModel=FLVER2.Read(shaders.Files.Single(f=>Leaf(f.Name).EndsWith(".flver",StringComparison.OrdinalIgnoreCase)).Bytes);
        render.BufferLayouts=shaderModel.BufferLayouts;render.GXLists=shaderModel.GXLists;
        render.Materials=shaderModel.Materials;render.Meshes=shaderModel.Meshes;
        var bake=SkinRetarget.Bake(prepared,render.Write(),textures,masks,HollowSourceSkin.Overrides(plan),target,"DS1R",preserveBoneLengths:true);
        if(bake.DirectWeightShare<.999999)throw new InvalidDataException("Source hollow visible skin lost an authored bone");
        if(muzzleIDs.Length>0)BindMuzzle(bake.Model,prepared,plan,profile);
        NonHumanoidEnemies.OrderRenderNodes(bake.Model);
        visual.Bytes=bake.Model.Write();skeleton.Bytes=plan.Skeleton;

        var clips=animation.Files.Where(f=>f.ID>=1000000000).ToArray();
        if(clips.Length!=HollowCombatProfiles.NativeClips||!motion.MotionSourceIDs.Keys.ToHashSet().SetEquals(clips.Select(f=>f.ID-1000000000)))
            throw new InvalidDataException("Native hollow motion coverage changed");
        var durations=clips.ToDictionary(f=>f.ID-1000000000,f=> {
            var pack=new NativeAnimation.Pack(f.Bytes.ToArray());return pack.Float(pack.Object("hkaSplineCompressedAnimation")+16);
        });
        var physicalClips=motion.MotionSourceIDs.Keys.ToDictionary(id=>id,id=>motion.Clip(id));
        var hidden=HollowSourceSkin.HiddenEquipmentBones(plan);
        var still=HollowSourceSkin.Sample(plan,reference,0);
        foreach(var file in clips) {
            int id=file.ID-1000000000;var sourceClip=physicalClips[id];
            file.Bytes=NativeRigAppend.Clip(file.Bytes.ToArray(),plan,
                time=>motion.ReferenceFallbackIDs.Contains(id)?still:HollowSourceSkin.Sample(plan,sourceClip,Math.Min(sourceClip.Duration,time*sourceClip.Duration)),
                preserveAddedRootWorld:true,allowZeroAddedBones:hidden);
        }
        var deathMotion=HollowDeathMotion.Apply(animation,plan,motion);
        var physics=character.Files.Single(f=>f.ID==300&&Leaf(f.Name).Equals(template+".hkx",StringComparison.OrdinalIgnoreCase));
        var death=HollowDeathRig.Convert(physics.Bytes.ToArray(),HollowDeathRig.ReadPhysics(readSource,profile.SourceModel),plan,profile.SourceModel);
        physics.Bytes=death.Physics;
        HollowCombatMotion.Report? eventReport=null;
        foreach(var file in animation.Files) {
            file.Name=file.Name.Replace(template,target,StringComparison.OrdinalIgnoreCase);
            if(TAE.Is(file.Bytes.ToArray())) {
                var tae=TAE.Read(file.Bytes);
                var actionDurations=motion.Mappings.ToDictionary(m=>m.NativeID,m=>durations[motion.NativeMotionID(m.NativeID)]);
                eventReport=HollowCombatMotion.Apply(tae,motion,actionDurations);
                tae.ID=200000+int.Parse(target[1..]);file.Bytes=tae.Write();
            } else if(file.ID is >=7000000 and <8000000)file.Bytes=SkinRetarget.RenameBehavior(file.Bytes,template,target);
        }
        if(eventReport==null)throw new InvalidDataException("Native hollow action timeline is missing");
        character.Files.RemoveAll(f=>Leaf(f.Name).EndsWith("_c.hkx",StringComparison.OrdinalIgnoreCase)||
            Leaf(f.Name).EndsWith(".clm2",StringComparison.OrdinalIgnoreCase)||Leaf(f.Name).Equals("div_data.txt",StringComparison.OrdinalIgnoreCase)||TPF.Is(f.Bytes.ToArray()));
        if(character.Files.Any(f=>f.ID==100))throw new InvalidDataException("Native hollow reserves the imported texture binder ID");
        character.Files.Add(new BinderFile(Binder.FileFlags.Flag1,100,@"N:\SPRJ\data\INTERROOT_ps4\chr\"+target+@"\"+target+".tpf",bake.Textures.Write(DCX.Type.None)));
        foreach(var file in character.Files)file.Name=file.Name.Replace(template,target,StringComparison.OrdinalIgnoreCase);
        character.Files.Sort((a,b)=>a.ID.CompareTo(b.ID));
        files.Add("chr/"+target+".chrbnd.dcx",character.Write(DCX.Type.DCX_DFLT_10000_44_9));
        files.Add("chr/"+target+".anibnd.dcx",animation.Write(DCX.Type.DCX_DFLT_10000_44_9));
        files.Add("action/script/"+target+".hks",readNative("action/script/"+template+".hks"));
        var active=eventReport.Actions.Where(a=>a.Hits>0||a.Shots>0).ToArray();
        return new(bake with{ProportionScale=1},new(profile.SourceModel,profile.Role.ToString(),template,plan.Original.Names.Length,plan.Added.Count,
            clips.Length,motion.ReferenceFallbackIDs.Count,active.Count(a=>a.Hits>0),active.Count(a=>a.Shots>0),muzzleIDs.Length==0?null:muzzleIDs[0],active,
            [..HollowCombatProfiles.Limitations,..eventReport.Limits,
                "The visible body and equipped meshes retain the complete selected source animation hierarchy. Source shield grips and equipment visibility are animated; source cape settling remains a static body-following approximation.",
                "Four previously shared death actions now own source-visible death clips. Their physical tracks use the checked local native death counterpart; their original native event timelines remain unchanged."]){DeathPhysics=death.Report,OwnedDeathClips=deathMotion});
    }
    static void BindMuzzle(FLVER2 target,byte[] sourceBytes,NativeRigAppend.Plan plan,HollowCombatProfiles.Profile profile) {
        var source=FLVER2.Read(sourceBytes);
        var original=source.Dummies.Single(d=>d.ReferenceID==profile.SourceProjectileDummy);
        int index=target.Dummies.FindIndex(d=>d.ReferenceID==profile.NativeProjectileDummy);
        if(index<0||target.Dummies.Count(d=>d.ReferenceID==profile.NativeProjectileDummy)!=1)
            throw new InvalidDataException("Native hollow projectile socket changed");
        target.Dummies[index]=HollowSourceSkin.RebindDummy(original,sourceBytes,plan,checked((short)profile.NativeProjectileDummy));
    }
}
