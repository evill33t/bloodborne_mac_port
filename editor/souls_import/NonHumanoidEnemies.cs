// SPDX-License-Identifier: GPL-3.0-or-later
using SoulsFormats;
using System.Numerics;

// Explicit source actors, not a generic cross-species retargeter. Visible bones
// retain source bind positions and source motion. Native bone indices, combat
// controllers and physical/attack anchors remain a separate, approximate layer.
internal static class NonHumanoidEnemies {
    internal sealed record Profile(string Source,string Native,int Npc,int Think,string Map,string Ai,int NativeBones,int Clips,string Variant="");
    internal static readonly Profile Boar=new("c3460","c1270",127500,127400,"m24_01_00_01","m24_01_00_00",163,76);
    internal static readonly Profile Demon=new("c2300","c3130",313000,313000,"m29_21_00_00/m29_21_00_46","m29_00_00_00",125,147);
    internal static readonly Profile Rat=new("c1201","c1100",110010,110000,"m24_01_00_01","m24_01_00_00",108,98);
    internal static Profile? Select(MSB1.Part.Enemy p,string map) {
        if(p.CharaInitID>=0||p.TalkID>0||p.InitAnimID!=-1)return null;
        if(p.ModelName=="c1201"&&p.EntityID==-1&&
            (map=="m10_02_00_00"&&p.Name=="c1201_0002"&&p.NPCParamID==120100&&p.ThinkParamID==120100||
             map=="m10_01_00_00"&&(p.Name is "c1201_0000" or "c1201_0001" or "c1201_0002")&&p.NPCParamID==120101&&p.ThinkParamID==120101))return Rat;
        return map=="m10_01_00_00"?(p.ModelName,p.NPCParamID,p.ThinkParamID,p.EntityID) switch {
            ("c3460",346000,346000,1010310)=>Boar,("c2300",230001,230003,1010340)=>Demon,_=>null}:null;
    }
    static string Leaf(string p)=>p.Replace('\\','/').Split('/').Last();
    // The native renderer requires active FLVER nodes in a contiguous prefix;
    // the Havok rig keeps its own original indices and binds these by name.
    // Appending in animation order interleaves disabled nubs and produces an
    // invalid render palette even when every animation track decodes correctly.
    internal static void OrderRenderNodes(FLVER2 model) {
        var original=model.Nodes.ToArray();var world=SkinRetarget.World(original);
        if(original.Length==0||original.Length>short.MaxValue||original.Select(n=>n.Name).Distinct(StringComparer.Ordinal).Count()!=original.Length)
            throw new InvalidDataException("Nonhumanoid render nodes must have unique bounded names");
        bool Active(FLVER.Node n)=>(n.Flags&FLVER.Node.NodeFlags.Disabled)==0;
        var order=Enumerable.Range(0,original.Length).OrderBy(i=>Active(original[i])?0:1).ToArray();var indices=new int[original.Length];
        for(int i=0;i<order.Length;i++)indices[order[i]]=i;
        int Remap(int value){if(value<0)return value;if(value>=indices.Length)throw new InvalidDataException("Nonhumanoid render reference exceeds its nodes");return indices[value];}
        short Link(short value)=>checked((short)Remap(value));
        foreach(var node in original){node.ParentIndex=Link(node.ParentIndex);node.FirstChildIndex=Link(node.FirstChildIndex);node.NextSiblingIndex=Link(node.NextSiblingIndex);node.PreviousSiblingIndex=Link(node.PreviousSiblingIndex);}
        model.Nodes=order.Select(i=>original[i]).ToList();int active=model.Nodes.Count(Active);
        foreach(var dummy in model.Dummies){dummy.ParentBoneIndex=Link(dummy.ParentBoneIndex);dummy.AttachBoneIndex=Link(dummy.AttachBoneIndex);}
        foreach(var mesh in model.Meshes) {
            int maximumBone=mesh.UseBoneWeights?SkinRetarget.MaximumBoneIndex(model,mesh):ushort.MaxValue;
            mesh.NodeIndex=Remap(mesh.NodeIndex);
            if(mesh.BoneIndices.Count>0)mesh.BoneIndices=mesh.BoneIndices.Select(Remap).ToList();
            else foreach(var vertex in mesh.Vertices) {
                if(mesh.UseBoneWeights)for(int i=0;i<4;i++)vertex.BoneIndices[i]=Remap(vertex.BoneIndices[i]);
                else vertex.NormalW=Remap(vertex.NormalW);
            }
            foreach(var vertex in mesh.Vertices)for(int i=0;i<(mesh.UseBoneWeights?4:1);i++) {
                if(mesh.UseBoneWeights&&vertex.BoneWeights[i]==0)continue;
                int bone=SkinRetarget.Bone(mesh,mesh.UseBoneWeights?vertex.BoneIndices[i]:vertex.NormalW);
                if(bone<0||bone>=active||bone>maximumBone)throw new InvalidDataException("Nonhumanoid skin references an inactive or unencodable render node");
            }
        }
        var after=SkinRetarget.World(model.Nodes);
        for(int i=0;i<original.Length;i++)if(world[i]!=after[indices[i]])throw new InvalidDataException("Nonhumanoid render permutation changed a named bind matrix");
    }
    internal static NativeRigAppend.Plan Rig(byte[] sourceModel,byte[] nativeModel,byte[] skeleton,SourceAnimation reference) {
        var rig=NativeAnimation.Skeleton(skeleton);var source=FLVER2.Read(sourceModel);var names=reference.Bones.Select((b,i)=>(b.Name,i)).ToDictionary(p=>p.Name,p=>p.i,StringComparer.Ordinal);
        if(reference.Bones.Count(b=>b.Parent<0)!=1||!reference.Bones[0].Name.Equals("Master",StringComparison.OrdinalIgnoreCase)||reference.Bones[0].Parent!=-1||!Matrix4x4.Invert(rig.Local[0],out var inverse))throw new InvalidDataException("Unsupported authored enemy source root");
        var added=reference.Bones.Select(b=> {
            var matches=source.Nodes.Where(n=>n.Name==b.Name).ToArray();if(matches.Length!=1)throw new InvalidDataException("Source mesh/animation bone mismatch: "+b.Name);
            var bind=new FLVER.Node(matches[0]){Flags=matches[0].Flags,Name="DS1_"+b.Name};
            bind.ParentIndex=bind.ParentIndex<0?(short)-1:names.TryGetValue(source.Nodes[bind.ParentIndex].Name,out int parent)?checked((short)(rig.Names.Length+parent)):throw new InvalidDataException("Source bind parent is outside the animation rig");
            var local=Matrix4x4.CreateScale(b.Scale)*Matrix4x4.CreateFromQuaternion(b.Rotation)*Matrix4x4.CreateTranslation(b.Translation);
            return new NativeRigAppend.Bone(bind.Name,b.Parent<0?0:rig.Names.Length+b.Parent,b.Parent<0?local*inverse:local,bind);
        }).ToArray();
        // Every weighted source bone must have an authored animation track or
        // reference transform. No inherited/clamped guessed rig mappings.
        foreach(var mesh in source.Meshes)foreach(var vertex in mesh.Vertices)for(int slot=0;slot<(mesh.Dynamic==1?4:1);slot++) {
            if(mesh.Dynamic==1&&vertex.BoneWeights[slot]==0)continue;
            int i=SkinRetarget.Bone(mesh,mesh.Dynamic==1?vertex.BoneIndices[slot]:vertex.NormalW);
            if(i<0||i>=source.Nodes.Count||!names.ContainsKey(source.Nodes[i].Name))throw new InvalidDataException("Source enemy has an unanimated weighted bone");
        }
        return NativeRigAppend.Create(nativeModel,skeleton,added);
    }
    // Shared numerical slots describe ordinary idle/locomotion/damage/death.
    // Extra native attacks explicitly reuse a source attack, retaining native
    // action timing and hitboxes. This does not claim the original moveset.
    internal static int SourceSlot(Profile profile,int native) {
        if(profile==Rat) {
            if(native<0||native/1000000 is not(0 or 10))throw new InvalidDataException("Unsupported native rat playback bank: "+native);
            int rat=native%1000000;
            return rat switch {
                0 or 200 or 201 or 202 or 203 or 500 or 502 or 503 or 602 or 603 or 604 or 605 or 701 or
                1500 or 1750 or 2000 or 2001 or 2002 or 2004 or 2005 or 2006 or 2007 or 2010 or 2011 or 2012 or 2013 or 2014 or
                2020 or 2030 or 2031 or 2032 or 2040 or 2050 or 2051 or 2052 or 2053 or 2200 or 2207 or 2209 or
                2250 or 3000 or 3001 or 3002 or 3003 or 3500=>rat,
                700 or 702 or 703=>701,1751=>1750,2003=>2002,2060=>2014,
                2080=>2004,2081=>2005,2082=>2006,2083=>2007,
                2202 or 2203 or 2204 or 2205 or 2211=>2200,2255=>2250,
                // Source 23xx/24xx reactions are additive. Preserve native
                // additive body tracks; appended tracks use identity deltas.
                // Any full-pose native variant uses this absolute source hit.
                >=2300 and <=2303 or >=2600 and <=2603 or >=2610 and <=2613 or >=2620 and <=2623 or >=2900 and <=2903=>2014,
                3004=>3000,3005=>3001,3006=>3002,3501=>3500,3600 or 3620=>3000,
                _=>throw new InvalidDataException("Unsupported native rat playback slot: "+native)
            };
        }
        int slot=native%1000000;
        if(profile==Boar) return slot switch {
            0 or 200 or 201 or 202 or 203 or 500 or 602 or 603 or 604 or 605 or 701 or 1500 or 1750 or
            2004 or 2005 or 2006 or 2007 or 2014 or 2020 or 2100 or 2200 or 2250 or 2500 or 2501 or
            >=3000 and <=3009 or 7000 or 7001=>slot,
            1751=>1750,2010 or 2011 or 2012 or 2013 or 2040 or 2060=>2014,2110=>2100,
            2202 or 2203 or 2205 or 2209 or 2220 or 2221=>2200,2255 or 2270 or 2271=>2250,
            2400 or 2410 or 2411 or 2412=>2020,2502 or 2510 or 2511 or 2512=>2500,
            2610 or 2611 or 2612 or 2613 or 2620 or 2621 or 2622 or 2623=>2020,
            3010=>3000,3011=>3001,3012=>3002,3014=>3004,3015=>3005,3016=>3006,3017=>3007,
            _=>throw new InvalidDataException("Unsupported native boar playback slot: "+native)
        };
        if(profile!=Demon)throw new InvalidDataException("Unknown nonhumanoid profile");
        return slot switch {
            0 or 200 or 201 or 202 or 203 or 602 or 603 or 604 or 605 or 701 or 1500 or 1750 or
            2004 or 2005 or 2006 or 2007 or 2010 or 2011 or 2012 or 2013 or 2020 or 2040 or 2200 or 2205 or 2250 or >=3000 and <=3009 or 3300 or 3301=>slot,
            500=>0,702 or 703=>701,2060=>2014,2255=>2250,
            2400 or >=2410 and <=2418 or 2550 or 2551 or 2552=>2020,
            3010 or 3020=>3000,3011 or 3021=>3001,3012=>3002,3013=>3003,3014=>3004,
            3025=>3005,3026=>3006,3030=>3007,
            7000 or 7001 or 7002 or 7010 or 7011 or 7012 or 8000 or 8010 or 8020 or 8030 or 8040 or 8200=>9000,
            _=>throw new InvalidDataException("Unsupported native Titanite playback slot: "+native)
        };
    }
    internal static byte[] Capsule(byte[] donor,PARAM.Row source,string sourceDefinitions) {
        // Both installed ParamDefs give these fields the same meaning and
        // four-byte prefix layout; keep every other native combat byte opaque.
        string path=Path.GetFullPath(Path.Combine(sourceDefinitions,"../../BB/Defs/NpcParam.xml"));
        var native=PARAMDEF.XmlDeserialize(path);
        string[] prefix=["behaviorVariationId","aiThinkId","nameId","turnVellocity","hitHeight","hitRadius","weight","hitYOffset"];
        var types=new[]{PARAMDEF.DefType.s32,PARAMDEF.DefType.s32,PARAMDEF.DefType.s32,PARAMDEF.DefType.f32,PARAMDEF.DefType.f32,PARAMDEF.DefType.f32,PARAMDEF.DefType.u32,PARAMDEF.DefType.f32};
        if(donor.Length!=388||!native.Fields.Take(8).Select(f=>f.InternalName).SequenceEqual(prefix)||!native.Fields.Take(8).Select(f=>f.DisplayType).SequenceEqual(types)||
            !source.Cells.Take(8).Select(c=>c.Def.InternalName).SequenceEqual(prefix)||!source.Cells.Take(8).Select(c=>c.Def.DisplayType).SequenceEqual(types))throw new InvalidDataException("Source/native capsule parameter layouts do not match");
        var bytes=donor.ToArray();
        foreach(var (field,offset,min,max) in new[]{("hitHeight",16,.1f,20f),("hitRadius",20,.05f,10f),("hitYOffset",28,-10f,10f)}) {
            float value=Convert.ToSingle(source.Cells.Single(c=>c.Def.InternalName==field).Value);
            if(!float.IsFinite(value)||value<min||value>max)throw new InvalidDataException("Source enemy capsule is outside the supported range");
            BitConverter.GetBytes(value).CopyTo(bytes,offset);
        }
        return bytes;
    }
    internal static SkinBake Package(Func<string,byte[]?> readSource,Func<string,byte[]> readNative,Profile profile,string target,IReadOnlySet<int> masks,Dictionary<string,byte[]> files,Func<int,int>? sourceSlot=null,Action<SkinBake>? adaptMaterials=null,Action<SkinBake,NativeRigAppend.Plan>? adaptRig=null) {
        var source=MapCharacters.Authored(readSource,profile.Source);var character=BND4.Read(readNative("chr/"+profile.Native+".chrbnd.dcx"));var animation=BND4.Read(readNative("chr/"+profile.Native+".anibnd.dcx"));
        var visual=character.Files.Single(f=>Leaf(f.Name).EndsWith(".flver",StringComparison.OrdinalIgnoreCase));var skeleton=animation.Files.Single(f=>f.ID==1000000);
        NativeRigAppend.ValidateCharacter(animation.Files.Single(f=>f.ID==7000400).Bytes.ToArray());
        var cache=new Dictionary<int,SourceAnimation>();SourceAnimation Clip(int id){if(!cache.TryGetValue(id,out var clip))cache.Add(id,clip=SourceAnimation.Load(readSource,profile.Source,id));return clip;}
        var plan=Rig(source.Model,visual.Bytes.ToArray(),skeleton.Bytes.ToArray(),Clip(0));
        if(plan.Original.Names.Length!=profile.NativeBones)throw new InvalidDataException("Native enemy donor rig changed");
        var overrides=plan.Added.ToDictionary(b=>b.Name[4..],b=>b.Name,StringComparer.Ordinal);
        // Use the already supported opaque character material/vertex ABI. The
        // pig donor only has SSS/fur shaders; those do not describe source armor.
        var render=FLVER2.Read(plan.Model);var shaderBank=BND4.Read(readNative("chr/c2610.chrbnd.dcx"));
        var shader=FLVER2.Read(shaderBank.Files.Single(f=>Leaf(f.Name).EndsWith(".flver",StringComparison.OrdinalIgnoreCase)).Bytes);
        render.BufferLayouts=shader.BufferLayouts;render.GXLists=shader.GXLists;render.Materials=shader.Materials;render.Meshes=shader.Meshes;
        var bake=SkinRetarget.Bake(source.Model,render.Write(),source.Textures,masks,overrides,target,"DS1R",preserveBoneLengths:true);
        if(bake.DirectWeightShare<.999999)throw new InvalidDataException("Authored enemy skin lost direct source bone weights");
        if(profile==Boar)BoarMaterials.Apply(readSource,source.Model,source.Textures,bake,target);
        adaptMaterials?.Invoke(bake);
        adaptRig?.Invoke(bake,plan);
        OrderRenderNodes(bake.Model);
        visual.Bytes=bake.Model.Write();skeleton.Bytes=plan.Skeleton;
        var clips=animation.Files.Where(f=>f.ID>=1000000000).ToArray();if(clips.Length!=profile.Clips)throw new InvalidDataException("Native enemy donor clip population changed");
        if(!Matrix4x4.Invert(plan.Original.Local[0],out var inverse))throw new InvalidDataException("Singular native locomotion root");
        foreach(var f in clips) {
            var sourceClip=Clip(sourceSlot?.Invoke(f.ID-1000000000)??SourceSlot(profile,f.ID-1000000000));
            try{f.Bytes=NativeRigAppend.Clip(f.Bytes.ToArray(),plan,time=>{var pose=sourceClip.Sample(Math.Min(sourceClip.Duration,time*sourceClip.Duration));pose[0]*=inverse;return pose;},preserveAddedRootWorld:true);}
            catch(InvalidDataException e){throw new InvalidDataException("Imported "+profile.Source+" playback "+profile.Native+"/"+(f.ID-1000000000)+": "+e.Message,e);}
        }
        character.Files.RemoveAll(f=>Leaf(f.Name).EndsWith("_c.hkx",StringComparison.OrdinalIgnoreCase)||Leaf(f.Name).EndsWith(".clm2",StringComparison.OrdinalIgnoreCase)||Leaf(f.Name).Equals("div_data.txt",StringComparison.OrdinalIgnoreCase)||TPF.Is(f.Bytes.ToArray()));
        if(character.Files.Any(f=>f.ID==100))throw new InvalidDataException("Native enemy reserves imported texture binder ID");
        character.Files.Add(new BinderFile(Binder.FileFlags.Flag1,100,@"N:\SPRJ\data\INTERROOT_ps4\chr\"+target+@"\"+target+".tpf",bake.Textures.Write(DCX.Type.None)));
        foreach(var f in character.Files)f.Name=f.Name.Replace(profile.Native,target,StringComparison.OrdinalIgnoreCase);
        foreach(var f in animation.Files) {
            f.Name=f.Name.Replace(profile.Native,target,StringComparison.OrdinalIgnoreCase);
            if(TAE.Is(f.Bytes.ToArray())){var tae=TAE.Read(f.Bytes);tae.ID=200000+int.Parse(target[1..]);f.Bytes=tae.Write();}
            else if(f.ID is >=7000000 and <8000000)f.Bytes=SkinRetarget.RenameBehavior(f.Bytes,profile.Native,target);
        }
        character.Files.Sort((a,b)=>a.ID.CompareTo(b.ID));files.Add("chr/"+target+".chrbnd.dcx",character.Write(DCX.Type.DCX_DFLT_10000_44_9));files.Add("chr/"+target+".anibnd.dcx",animation.Write(DCX.Type.DCX_DFLT_10000_44_9));files.Add("action/script/"+target+".hks",readNative("action/script/"+profile.Native+".hks"));
        return bake with{ProportionScale=1};
    }
}
