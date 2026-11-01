// SPDX-License-Identifier: GPL-3.0-or-later
using SoulsFormats;
using System.Numerics;

// The two authored c2510 appearances have a separate visible source skeleton.
// Doll playback/physics remain native; no Doll AI or quest script is imported.
internal static class MapMerchants {
    internal sealed record Report(string SourceMap,string SourceName,int SourceEntityID,int SourceNPCParamID,int SourceThinkParamID,int SourceTalkID,int SourceInitAnimID,
        string TargetModel,int EntityID,int NPCParamID,string CollisionName,int[] DisplayMasks,int Vertices,int Triangles,int NativeBones,int SourceBones,
        MapCharacterAnimations.Pose SourcePose,IReadOnlyList<string> SourceParts,IReadOnlyList<string> TextureFallbacks,IReadOnlyList<string> Limitations);
    internal sealed record Export(IReadOnlyDictionary<string,byte[]> Files,IReadOnlyList<MSBB.Model.Enemy> Models,IReadOnlyList<MSBB.Part.Enemy> Actors,IReadOnlyList<Report> Reports,byte[] GameParamBytes);
    static readonly int[] IdleSlots=[0,1000000,2000000];
    const string Template="c8030",SourceModel="c2510";
    static string Leaf(string path)=>path.Replace('\\','/').Split('/').Last();
    static bool Finite(Vector3 value)=>float.IsFinite(value.X)&&float.IsFinite(value.Y)&&float.IsFinite(value.Z);

    internal static Export Bake(Func<string,byte[]?> readSource,Func<string,byte[]?> readNative,string profile,string sourceMap,string definitions,
        string nativeRoot,MSBB destination,Vector3 offset,IReadOnlyList<MapCollisionBindings.Chunk> chunks,IReadOnlyList<MSBB.Part.Collision> slots,byte[] gameParamBytes) {
        if(profile!="DS1R"||sourceMap is not("m10_01_00_00" or "m10_02_00_00"))throw new NotSupportedException("Merchant appearance requires the verified DS1R Burg or Firelink contract");
        if(!Finite(offset)||chunks.Count==0||chunks.Count!=slots.Count||slots.Any(s=>destination.Parts.Collisions.Count(c=>c.Name==s.Name)!=1))
            throw new InvalidDataException("Merchant import requires paired, registered collision regions");
        byte[] Source(string path)=>readSource(path)??throw new FileNotFoundException("Source merchant asset unavailable",path);
        byte[] Native(string path)=>readNative(path)??throw new FileNotFoundException("Native merchant asset unavailable",path);
        bool female=sourceMap=="m10_02_00_00";int sourceEntity=female?6240:6230,sourceNpc=female?251000:251001,sourceThink=female?251001:251000,sourceTalk=female?102624:101623,idle=female?0:9000;
        int modelID=female?8903:8904,entity=female?2419703:4039700,paramID=modelID*100;string target="c"+modelID,name=target+"_0000";
        var source=MSB1.Read(readSource($"map/MapStudio/{sourceMap}.msb")??Source($"map/MapStudio/{sourceMap}.msb.dcx"));
        var p=source.Parts.Enemies.Single(e=>e.EntityID==sourceEntity);
        if(p.ModelName!=SourceModel||p.NPCParamID!=sourceNpc||p.ThinkParamID!=sourceThink||p.TalkID!=sourceTalk||p.CharaInitID!=-1||p.InitAnimID!=(female?-1:9000)||
            !Finite(p.Position)||!Finite(p.Rotation)||!Finite(p.Scale)||p.Scale!=Vector3.One)
            throw new InvalidDataException("Authored merchant identity/transform contract changed");
        var game=BND4.Read(gameParamBytes);var npcFile=game.Files.Single(f=>Leaf(f.Name)=="NpcParam.param");var npcs=MapCharacters.NativeRows(npcFile.Bytes,false);
        // Validate both the composed overlay and the original asset namespace;
        // optional readers must return null for genuinely absent owned assets.
        var paths=new[]{"chr/"+target+".chrbnd.dcx","chr/"+target+".chrbnd","chr/"+target+".anibnd.dcx","chr/"+target+".anibnd","chr/"+target+".chrtpfbhd","chr/"+target+".chrtpfbdt","chr/"+target+".texbnd.dcx","chr/"+target+".tpf.dcx","action/script/"+target+".hks"}
            .Concat(new[]{".fev",".itl",".fsb","_ded.fsb","_eng.fsb","_esa.fsb","_ese.fsb","_frf.fsb","_iti.fsb","_jaj.fsb","_ptb.fsb"}.Select(s=>"sound/sprj_"+target+s));
        if(npcs.Rows.Any(r=>r.ID==paramID)||destination.Models.GetEntries().Any(m=>m.Name.Equals(target,StringComparison.OrdinalIgnoreCase))||
            destination.Parts.GetEntries().Any(a=>a.Name==name||a.ModelName==target)||
            destination.Parts.GetEntries().Select(a=>a.EntityID).Concat(destination.Events.GetEntries().Select(e=>e.EntityID)).Concat(destination.Regions.GetEntries().Select(r=>r.EntityID)).Contains(entity)||
            paths.Any(path=>readNative(path)!=null||File.Exists(Path.Combine(nativeRoot,path)))||
            Directory.EnumerateFiles(Path.Combine(nativeRoot,"chr"),target+".*").Any())
            throw new InvalidDataException("Owned merchant model, entity, parameter or asset namespace is occupied");
        var sourceGame=BND3.Read(Source("param/GameParam/GameParam.parambnd.dcx"));var sourceRows=PARAM.Read(sourceGame.Files.Single(f=>Leaf(f.Name)=="NpcParam.param").Bytes);
        sourceRows.ApplyParamdef(PARAMDEF.XmlDeserialize(Path.Combine(definitions,"NpcParam.xml")));
        var row=sourceRows.Rows.Single(r=>r.ID==sourceNpc);var masks=Enumerable.Range(0,16).Where(i=>Convert.ToInt32(row.Cells.Single(c=>c.Def.InternalName=="modelDispMask"+i).Value)!=0).ToHashSet();
        if(!masks.SetEquals(female?Array.Empty<int>():new[]{0}))throw new InvalidDataException("Source merchant display masks changed");
        int collisionSlot=MapCollisionBindings.ObjectSlot(chunks,p.CollisionName,p.Position+offset);var collision=slots[collisionSlot];
        var donorMap=MSBB.Read(Native("map/MapStudio/m21_00_00_00.msb.dcx"));
        var donor=donorMap.Parts.Enemies.Single(a=>a.ModelName==Template&&a.NPCParamID==803000&&a.ThinkParamID==803000);
        var donorModel=donorMap.Models.Enemies.Single(m=>m.Name==Template);
        var appearance=MapCharacters.Authored(readSource,SourceModel);var files=new Dictionary<string,byte[]>();
        var loaded=MapCharacterAnimations.Load(readSource,SourceModel,idle);
        var skin=Package(readSource,Native,appearance.Model,appearance.Textures,masks,target,loaded.Animation,files);
        var donorRow=npcs.Rows.Single(r=>r.ID==803000);var payload=MapCharacters.Payload(donorRow);
        var nativeDef=PARAMDEF.XmlDeserialize(Path.GetFullPath(Path.Combine(definitions,"../../BB/Defs/NpcParam.xml")));
        int field=nativeDef.Fields.FindIndex(f=>f.InternalName=="normalChangeAnimChrId");var prefix=new PARAMDEF();if(field>=0)prefix.Fields.AddRange(nativeDef.Fields.Take(field));
        if(nativeDef.ParamType!="NPC_PARAM_ST"||nativeDef.GetRowSize()!=388||field<0||prefix.GetRowSize()!=0x162||nativeDef.Fields[field].DisplayType!=PARAMDEF.DefType.s16||
            nativeDef.Fields[field].ArrayLength!=1||nativeDef.Fields[field].BitSize!=-1||BitConverter.ToInt16(payload,0x162)!=-1||BitConverter.ToInt32(payload,4)!=803000)
            throw new InvalidDataException("Native friendly animation-owner/AI parameter contract changed");
        BitConverter.GetBytes(-1).CopyTo(payload,4);payload[0x149]&=0xDF;
        foreach(int at in new[]{0x146,0x147,0x14A,0x14B})payload[at]=255;
        MapCharacters.CloneRow(npcs,donorRow,paramID,"Imported source merchant "+sourceEntity,payload);
        npcFile.Bytes=MapCharacters.WriteRows(npcs);var gameBytes=game.Write(DCX.Type.DCX_DFLT_10000_44_9);files.Add("param/gameparam/gameparam.parambnd.dcx",gameBytes);
        var model=(MSBB.Model.Enemy)donorModel.DeepCopy();model.Name=target;model.SibPath=model.SibPath.Replace(Template,target,StringComparison.OrdinalIgnoreCase);
        var actor=(MSBB.Part.Enemy)donor.DeepCopy();actor.Name=name;actor.ModelName=target;actor.EntityID=entity;actor.NPCParamID=paramID;actor.ThinkParamID=-1;actor.TalkID=-1;actor.CharaInitID=-1;
        actor.Position=p.Position+offset;actor.Rotation=p.Rotation;actor.Scale=p.Scale;actor.CollisionName=collision.Name;actor.PatrolInfoName=null!;Array.Clear(actor.MovePointNames);
        actor.InitAnimID=-1;actor.DamageAnimID=-1;actor.PatrolType=0;actor.FallbackPlatoonID=0;
        Array.Copy(collision.DrawGroups,actor.DrawGroups,actor.DrawGroups.Length);if(actor.DrawGroups.All(v=>v==0))Array.Fill(actor.DrawGroups,uint.MaxValue);
        Array.Copy(collision.DispGroups,actor.DispGroups,actor.DispGroups.Length);if(actor.DispGroups.All(v=>v==0))Array.Copy(actor.DrawGroups,actor.DispGroups,actor.DispGroups.Length);
        // Source merchants have zero draw/disp masks. Keep the actual floor's
        // residency groups and the native friendly backread contract instead.
        var limits=new[]{"Authored source c2510 geometry, display masks and idle motion are retained on a separate visible skeleton. Native friendly capsule/hitboxes remain an approximation.",
            "Native reaction and interaction states retain their own timing while sampling the source idle. Source attacks, cloth physics, quest-driven poses and availability are not ported by this appearance adapter.",
            "Think and NPC AI are disabled; no native Doll quest AI is copied. Dialogue and shop access are composed separately. Native gameplay appearance/idle-loop validation is still required."};
        var pose=new MapCharacterAnimations.Pose(SourceModel,idle,loaded.Archive,loaded.Animation.Duration,loaded.Animation.FrameCount,IdleSlots,false,limits);
        var report=new Report(sourceMap,p.Name,p.EntityID,p.NPCParamID,p.ThinkParamID,p.TalkID,p.InitAnimID,target,entity,paramID,collision.Name,masks.Order().ToArray(),skin.Vertices,skin.Triangles,129,loaded.Animation.Bones.Count,pose,appearance.Paths,skin.TextureFallbacks,limits);
        return new(files,[model],[actor],[report],gameBytes);
    }

    static SkinBake Package(Func<string,byte[]?> readSource,Func<string,byte[]> readNative,byte[] sourceModel,List<TPF.Texture> textures,IReadOnlySet<int> masks,string target,SourceAnimation idle,Dictionary<string,byte[]> files) {
        var character=BND4.Read(readNative("chr/"+Template+".chrbnd.dcx"));var animation=BND4.Read(readNative("chr/"+Template+".anibnd.dcx"));
        var visual=character.Files.Single(f=>Leaf(f.Name).EndsWith(".flver",StringComparison.OrdinalIgnoreCase));var skeleton=animation.Files.Single(f=>f.ID==1000000);
        NativeRigAppend.ValidateCharacter(animation.Files.Single(f=>f.ID==7000400).Bytes.ToArray());
        var plan=NonHumanoidEnemies.Rig(sourceModel,visual.Bytes.ToArray(),skeleton.Bytes.ToArray(),SourceAnimation.Load(readSource,SourceModel,0));
        if(plan.Original.Names.Length!=129||plan.Added.Count!=56||plan.Count!=185)throw new InvalidDataException("Source/native merchant rig population changed");
        var shaderBank=BND4.Read(readNative("chr/c2610.chrbnd.dcx"));var shader=FLVER2.Read(shaderBank.Files.Single(f=>Leaf(f.Name).EndsWith(".flver",StringComparison.OrdinalIgnoreCase)).Bytes);
        var render=FLVER2.Read(plan.Model);render.BufferLayouts=shader.BufferLayouts;render.GXLists=shader.GXLists;render.Materials=shader.Materials;render.Meshes=shader.Meshes;
        var skin=SkinRetarget.Bake(sourceModel,render.Write(),textures,masks,plan.Added.ToDictionary(b=>b.Name[4..],b=>b.Name,StringComparer.Ordinal),target,"DS1R",preserveBoneLengths:true);
        if(skin.DirectWeightShare<.999999)throw new InvalidDataException("Merchant appearance lost direct source bone weights");
        NonHumanoidEnemies.OrderRenderNodes(skin.Model);visual.Bytes=skin.Model.Write();skeleton.Bytes=plan.Skeleton;
        var clips=animation.Files.Where(f=>f.ID>=1000000000).ToArray();if(clips.Length!=61||!Matrix4x4.Invert(plan.Original.Local[0],out var inverse))throw new InvalidDataException("Native friendly playback contract changed");
        var oldDurations=new Dictionary<int,float>();
        foreach(var file in clips) {
            int id=file.ID-1000000000;var bytes=file.Bytes.ToArray();var pack=new NativeAnimation.Pack(bytes);float old=pack.Float(pack.Object("hkaSplineCompressedAnimation")+16);
            bytes=NativeRigAppend.Clip(bytes,plan,time=>{var pose=idle.Sample(Math.Min(idle.Duration,time*idle.Duration));pose[0]*=inverse;return pose;},preserveAddedRootWorld:true);
            if(IdleSlots.Contains(id)) {oldDurations.Add(id,old);bytes=RetimeIdle(bytes,idle.Duration);}
            file.Bytes=bytes;
        }
        var taeFile=animation.Files.Single(f=>TAE.Is(f.Bytes.ToArray()));var tae=TAE.Read(taeFile.Bytes);
        foreach(int slot in IdleSlots) {
            var row=tae.Animations.Single(a=>a.ID==slot);string stem=$"a{slot/1000000:D3}_000000";
            if(row.AnimFileName!=stem+".hkt"||Leaf(clips.Single(f=>f.ID==1000000000+slot).Name)!=stem+".hkx")throw new InvalidDataException("Native merchant idle slot changed");
            row.Events.RemoveAll(DollIdleEvent);
            foreach(var e in row.Events) {
                if(!float.IsFinite(e.StartTime)||!float.IsFinite(e.EndTime)||e.StartTime<0||e.EndTime<e.StartTime)throw new InvalidDataException("Invalid friendly idle event timing");
                e.StartTime=Math.Clamp(e.StartTime/oldDurations[slot],0,1)*idle.Duration;e.EndTime=Math.Clamp(e.EndTime/oldDurations[slot],0,1)*idle.Duration;
            }
        }
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
    static bool DollIdleEvent(TAE.Event e) {
        var b=e.GetParameterBytes(false);int I(int at)=>BitConverter.ToInt32(b,at);
        if(e.Type==66&&b.Length==16&&I(0)==160&&b.AsSpan(4).IndexOfAnyExcept((byte)0)<0)return true;
        if(e.Type==67&&b.Length==16&&I(0)==153&&b.AsSpan(4).IndexOfAnyExcept((byte)0)<0)return true;
        if(e.Type==128&&b.Length==16&&I(0)==1&&I(4)==803006500&&b.AsSpan(8).IndexOfAnyExcept((byte)0)<0)return true;
        if(e.Type==313&&b.Length==16&&I(0)==0&&I(4)==0x3f800000&&I(8)==0x3f800000&&I(12)==0)return true;
        return e.Type==315&&b.Length==48&&I(16) is >=0 and <=5&&I(20)==0&&I(40)==0&&I(44)==0&&new[]{0,4,8,12,24,28,32,36}.All(at=>I(at)==unchecked((int)0xbf800000));
    }
}
