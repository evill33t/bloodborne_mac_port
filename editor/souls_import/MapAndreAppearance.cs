// SPDX-License-Identifier: GPL-3.0-or-later
using SoulsFormats;
using System.Numerics;
using System.Security.Cryptography;

// c2640 is an authored character, including Andre's head and smithing tools.
// A separate complete visible source rig preserves his pose and hammer bones;
// the native friendly wrapper retains its own physical/controller rig.
internal static class MapAndreAppearance {
    const string Template="c8030",SourceModel="c2640";
    static readonly int[] IdleSlots=[0,1000000,2000000];
    internal sealed record Result(SkinBake Skin,int NativeBones,int SourceBones,int NativeClips,string SourceArchive,int SourceFrames,float SourceDuration,IReadOnlyList<string> SourcePaths);
    static string Leaf(string p)=>p.Replace('\\','/').Split('/').Last();
    internal static Result Apply(Func<string,byte[]?> source,Func<string,byte[]> native,string definitions,Dictionary<string,byte[]> files) {
        byte[] Read(string p)=>source(p)??throw new FileNotFoundException("Andre authored source unavailable",p);
        if(Convert.ToHexStringLower(SHA256.HashData(Read("chr/c2640.chrbnd.dcx")))!="719ddd381ad3dd06821e87a1e09870f70903018237e0821d77d50ee2222bc1c1"||
            Convert.ToHexStringLower(SHA256.HashData(Read("chr/c2640.anibnd.dcx")))!="f31be0969a9b757eb5c324e62b2c8f6ea83fa4c30d45ad81f009f78b2a8199c4")throw new InvalidDataException("Andre source character/animation changed");
        var game=BND3.Read(Read("param/GameParam/GameParam.parambnd.dcx"));var npc=PARAM.Read(game.Files.Single(f=>Leaf(f.Name)=="NpcParam.param").Bytes);npc.ApplyParamdef(PARAMDEF.XmlDeserialize(Path.Combine(definitions,"NpcParam.xml")));var row=npc.Rows.Single(r=>r.ID==264000);
        var masks=Enumerable.Range(0,16).Where(i=>Convert.ToInt32(row["modelDispMask"+i].Value)!=0).ToHashSet();if(masks.Count!=0)throw new InvalidDataException("Andre source display mask changed");
        var appearance=MapCharacters.Authored(source,SourceModel);var loaded=MapCharacterAnimations.Load(source,SourceModel,9000);
        if(loaded.Animation.Bones.Count!=155||loaded.Animation.FrameCount!=36||Math.Abs(loaded.Animation.Duration-1.166667f)>.00001f)throw new InvalidDataException("Andre hammering animation contract changed");
        var skin=Package(source,native,appearance.Model,appearance.Textures,masks,"c8915",loaded.Animation,files);
        return new(skin,129,155,61,loaded.Archive,loaded.Animation.FrameCount,loaded.Animation.Duration,appearance.Paths);
    }
    static SkinBake Package(Func<string,byte[]?> readSource,Func<string,byte[]> readNative,byte[] sourceModel,List<TPF.Texture> textures,IReadOnlySet<int> masks,string target,SourceAnimation idle,Dictionary<string,byte[]> files) {
        var character=BND4.Read(readNative("chr/"+Template+".chrbnd.dcx"));var animation=BND4.Read(readNative("chr/"+Template+".anibnd.dcx"));
        var visual=character.Files.Single(f=>Leaf(f.Name).EndsWith(".flver",StringComparison.OrdinalIgnoreCase));var skeleton=animation.Files.Single(f=>f.ID==1000000);
        NativeRigAppend.ValidateCharacter(animation.Files.Single(f=>f.ID==7000400).Bytes.ToArray());
        var plan=NonHumanoidEnemies.Rig(sourceModel,visual.Bytes.ToArray(),skeleton.Bytes.ToArray(),SourceAnimation.Load(readSource,SourceModel,0));
        if(plan.Original.Names.Length!=129||plan.Added.Count!=155||plan.Count!=284)throw new InvalidDataException("Source/native Andre rig population changed");
        var shaderBank=BND4.Read(readNative("chr/c2610.chrbnd.dcx"));var shader=FLVER2.Read(shaderBank.Files.Single(f=>Leaf(f.Name).EndsWith(".flver",StringComparison.OrdinalIgnoreCase)).Bytes);
        var render=FLVER2.Read(plan.Model);
        // The combined playback rig has284 tracks, but the retail character
        // vertex layout stores byte bone indices. Compact active render nodes
        // before name-based skin mapping; Havok's native/source indices remain
        // unchanged and the renderer resolves those tracks by bone name.
        // Retail Doll geometry contains special disabled-node attachments;
        // it is not the skinning template and is replaced below in full.
        render.Meshes.Clear();NonHumanoidEnemies.OrderRenderNodes(render);
        render.BufferLayouts=shader.BufferLayouts;render.GXLists=shader.GXLists;render.Materials=shader.Materials;render.Meshes=shader.Meshes;
        var skin=SkinRetarget.Bake(sourceModel,render.Write(),textures,masks,plan.Added.ToDictionary(b=>b.Name[4..],b=>b.Name,StringComparer.Ordinal),target,"DS1R",preserveBoneLengths:true);
        if(skin.DirectWeightShare<.999999)throw new InvalidDataException("Andre appearance lost direct source bone weights");
        NonHumanoidEnemies.OrderRenderNodes(skin.Model);visual.Bytes=skin.Model.Write();skeleton.Bytes=plan.Skeleton;
        var clips=animation.Files.Where(f=>f.ID>=1000000000).ToArray();if(clips.Length!=61||!Matrix4x4.Invert(plan.Original.Local[0],out var inverse))throw new InvalidDataException("Native friendly playback contract changed");
        foreach(var file in clips) {
            int id=file.ID-1000000000;var bytes=file.Bytes.ToArray();
            bytes=NativeRigAppend.Clip(bytes,plan,time=>{var pose=idle.Sample(Math.Min(idle.Duration,time*idle.Duration));pose[0]*=inverse;return pose;},preserveAddedRootWorld:true);
            if(IdleSlots.Contains(id))bytes=RetimeIdle(bytes,idle.Duration);
            file.Bytes=bytes;
        }
        var taeFile=animation.Files.Single(f=>TAE.Is(f.Bytes.ToArray()));var tae=TAE.Read(taeFile.Bytes);
        foreach(var row in tae.Animations){row.Events.Clear();row.EventGroups.Clear();}
        tae.ID=200000+int.Parse(target[1..]);taeFile.Bytes=tae.Write();
        character.Files.RemoveAll(f=>Leaf(f.Name).EndsWith("_c.hkx",StringComparison.OrdinalIgnoreCase)||Leaf(f.Name).EndsWith(".clm2",StringComparison.OrdinalIgnoreCase)||Leaf(f.Name).Equals("div_data.txt",StringComparison.OrdinalIgnoreCase)||TPF.Is(f.Bytes.ToArray()));
        if(character.Files.Any(f=>f.ID==100))throw new InvalidDataException("Native friendly wrapper reserves texture binder ID100");
        character.Files.Add(new BinderFile(Binder.FileFlags.Flag1,100,@"N:\SPRJ\data\INTERROOT_ps4\chr\"+target+@"\"+target+".tpf",skin.Textures.Write(DCX.Type.None)));
        foreach(var file in character.Files)file.Name=file.Name.Replace(Template,target,StringComparison.OrdinalIgnoreCase);
        foreach(var file in animation.Files){file.Name=file.Name.Replace(Template,target,StringComparison.OrdinalIgnoreCase);if(file.ID is >=7000000 and <8000000)file.Bytes=SkinRetarget.RenameBehavior(file.Bytes,Template,target);}
        character.Files.Sort((a,b)=>a.ID.CompareTo(b.ID));files.Add("chr/"+target+".chrbnd.dcx",character.Write(DCX.Type.DCX_DFLT_10000_44_9));
        files.Add("chr/"+target+".anibnd.dcx",animation.Write(DCX.Type.DCX_DFLT_10000_44_9));files.Add("action/script/"+target+".hks",readNative("action/script/"+Template+".hks"));return skin with{ProportionScale=1};
    }
    static byte[] RetimeIdle(byte[] bytes,float duration) {
        var edit=new NativeRigAppend.Edit(bytes);var p=edit.Pack;int a=p.Object("hkaSplineCompressedAnimation");int frames=p.Int(a+56);
        if(duration<=0||frames<2||p.Int(a+60)!=1||p.Int(a+64)!=256)throw new InvalidDataException("Unsupported friendly idle timing");
        // The shipped three Doll idles have no extracted root-motion object.
        // Reject a changed donor rather than leaving its timing out of sync.
        int header=64+BitConverter.ToUInt16(bytes,62)+128,global=BitConverter.ToInt32(bytes,header+28),virtuals=BitConverter.ToInt32(bytes,header+32);
        for(int at=global;at+12<=virtuals;at+=12)if(BitConverter.ToInt32(bytes,p.Data+at)==a+32)throw new InvalidDataException("Friendly idle unexpectedly has extracted root motion");
        float dt=duration/(frames-1),block=255*dt;foreach(var (at,value) in new[]{(a+16,duration),(a+72,block),(a+76,1/block),(a+80,dt)})edit.Int(at,BitConverter.SingleToInt32Bits(value));return edit.Finish();
    }
}
