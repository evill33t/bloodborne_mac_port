// SPDX-License-Identifier: GPL-3.0-or-later
using SoulsFormats;
using System.Numerics;
using System.Security.Cryptography;
using System.Text;

// An independently spawned source head, not a scaled piece of the body skin.
// The encounter owns its placement, source sockets and lifetime/cleanup.
internal static class HydraHeads {
    internal const string Model="c8831";
    internal const int Npc=883001,Think=883001,FallAnimation=7005;
    internal sealed record Motion(int NativeAnimation,int SourceAnimation,int Frames,float Duration,int Tracks);
    internal sealed record Fade(float Start,float End,float From,float To);
    internal sealed record Report(string SourceModel,string TargetModel,string TextureOwner,int Vertices,int Triangles,int NativeClips,
        IReadOnlyList<Motion> FallingMotions,IReadOnlyList<Fade> Fades,HydraHeadsPhysics.Report Physics,
        IReadOnlyDictionary<string,string> InputSHA256,IReadOnlyList<string> Limitations);
    internal sealed record Result(IReadOnlyDictionary<string,byte[]> Files,SkinBake Skin,Report Report);
    static string Leaf(string p)=>p.Replace('\\','/').Split('/').Last();
    static string Hash(byte[] value)=>Convert.ToHexStringLower(SHA256.HashData(value));
    static void Need(bool value,string why){if(!value)throw new InvalidDataException("Hydra severed head: "+why);}

    internal static Result Package(Func<string,byte[]?> readSource,Func<string,byte[]> readNative,string target=Model) {
        Need(target==Model,"only the owned c8831 model is allocated");
        var hashes=new Dictionary<string,string>(StringComparer.Ordinal);
        byte[]? Source(string path){var bytes=readSource(path);if(bytes!=null)hashes.TryAdd("DS1R/"+path,Hash(bytes));return bytes;}
        byte[] Native(string path){var bytes=readNative(path);hashes.TryAdd("BB/"+path,Hash(bytes));return bytes;}
        var source=HydraHeadsSource.Load(Source);
        var clip=SourceAnimation.Load(Source,"c3531",8100);
        Need(clip.Bones.Count==14&&clip.FrameCount==151&&Math.Abs(clip.Duration-5)<.0001f,"source 8100 dimensions/timing changed");
        var sourceAnimation=BND3.Read(Source("chr/c3531.anibnd.dcx")!);
        var sourceTaeFile=sourceAnimation.Files.Single(f=>f.ID==3000000);
        Need(Hash(sourceTaeFile.Bytes.ToArray())=="190bc65e87e4ef679b4abd914fae63aa13b42af805a7450203fd6b40f4ef478c","source TAE changed");
        var sourceTae=TAE.Read(sourceTaeFile.Bytes);var sourceFall=sourceTae.Animations.Single(a=>a.ID==8100);
        var sourceFades=sourceFall.Events.Where(e=>e.Type==193).ToArray();
        Need(sourceFades.Length==2&&sourceFades.All(e=>e.GetParameterBytes(false).Length==8),"source opacity payload changed");
        var fades=sourceFades.Select(e=>new Fade(e.StartTime,e.EndTime,BitConverter.ToSingle(e.GetParameterBytes(false)),BitConverter.ToSingle(e.GetParameterBytes(false),4))).ToArray();
        Need(Math.Abs(fades[0].Start-1.5f)<.001f&&Math.Abs(fades[0].End-7f/3)<.001f&&fades[0].From==1&&fades[0].To==0&&fades[1].From==0&&fades[1].To==0,"source fade contract changed");
        var files=new Dictionary<string,byte[]>();NativeRigAppend.Plan? rig=null;
        var profile=new NonHumanoidEnemies.Profile("c3531","c2730",273100,273100,"m24_00_00_00","m24_00_00_00",110,159,"hydra-severed-head");
        var skin=NonHumanoidEnemies.Package(Source,Native,profile,target,new HashSet<int>(),files,
            sourceSlot:id=>id%1000000==FallAnimation?8100:0,adaptRig:(_,plan)=>rig=plan);
        var plan=rig??throw new InvalidDataException("Missing owned head rig");HydraHeadsPhysics.ValidatePlan(source,plan);
        var bank=BND4.Read(files["chr/"+target+".chrbnd.dcx"]);
        var physicsFile=bank.Files.Single(f=>Leaf(f.Name).Equals(target+".hkx",StringComparison.OrdinalIgnoreCase));
        var pwvFile=bank.Files.Single(f=>Leaf(f.Name)==target+".hkxpwv");
        var physics=HydraHeadsPhysics.Convert(physicsFile.Bytes.ToArray(),source,plan);
        physicsFile.Bytes=physics.Physics;pwvFile.Bytes=HydraHeadsPhysics.Pwv(pwvFile.Bytes.ToArray(),source,plan);
        files["chr/"+target+".chrbnd.dcx"]=bank.Write(DCX.Type.DCX_DFLT_10000_44_9);

        var animation=BND4.Read(files["chr/"+target+".anibnd.dcx"]);
        var nativeTae=TAE.Read(animation.Files.Single(f=>f.ID==3000000).Bytes);
        // Verify the payload against an actual shipped fade, not just XML.
        var opacity=nativeTae.Animations.Single(a=>a.ID==2200).Events.Where(e=>e.Type==193).ToArray();
        Need(opacity.Length==3&&opacity.All(e=>e.Group?.GroupType==192&&e.GetParameterBytes(false).Length==16&&e.GetParameterBytes(false).AsSpan(8).IndexOfAnyExcept((byte)0)<0)&&
            opacity.Any(e=>BitConverter.ToSingle(e.GetParameterBytes(false))==1&&BitConverter.ToSingle(e.GetParameterBytes(false),4)==0),"native opacity precedent changed");
        var fallFlag=nativeTae.Animations.Single(a=>a.ID==701).Events.Single(e=>e.Type==0&&BitConverter.ToInt32(e.GetParameterBytes(false))==51).GetParameterBytes(false);
        Need(Convert.ToHexString(fallFlag)=="3300000000000000FFFFFFFF00000000","native disable-fall flag precedent changed");
        // No donor attacks, sounds, effects, animation switches or death flags.
        foreach(var a in nativeTae.Animations){a.Events.Clear();a.EventGroups.Clear();}
        Need(Matrix4x4.Invert(plan.Original.Local[0],out var inverse),"singular controller root");
        var skeleton=NativeAnimation.Skeleton(plan.Skeleton);var samples=new Matrix4x4[plan.Count*clip.FrameCount];
        // Retail c2730 names one bone "Jaw\n". Keep that shipped identity in
        // the final annotation without weakening the general spline encoder.
        Need(skeleton.Names.Where(n=>n.Any(char.IsControl)).SequenceEqual(new[]{"Jaw\n"}),"native control-character name precedent changed");
        for(int frame=0;frame<clip.FrameCount;frame++) {
            var pose=clip.Sample(frame*clip.FrameDuration);pose[0]*=inverse;
            Array.Copy(plan.Original.Local,0,samples,frame*plan.Count,110);Array.Copy(pose,0,samples,frame*plan.Count+110,14);
        }
        var falling=new List<Motion>();
        foreach(var f in animation.Files.Where(f=>f.ID>=1000000000&&(f.ID-1000000000)%1000000==FallAnimation)) {
            int id=f.ID-1000000000;
            var encoded=NativeSpline.Encode(samples,plan.Count,clip.FrameCount,clip.FrameDuration,skeleton.Name,
                Enumerable.Range(0,plan.Count).Select(i=>"HydraHeadTrack_"+i).ToArray());
            var edit=new NativeRigAppend.Edit(encoded);var annotations=edit.Pack.Array(edit.Pack.Object("hkaSplineCompressedAnimation")+40,24,128);
            Need(annotations.Count==plan.Count,"encoded annotation population differs");
            for(int i=0;i<plan.Count;i++)edit.Pointer(annotations.At+i*24,edit.Append(Encoding.UTF8.GetBytes(skeleton.Names[i]+'\0'),2));
            f.Bytes=edit.Finish();
            var a=nativeTae.Animations.Single(a=>a.ID==id);var flagGroup=new TAE.EventGroup(0);var fadeGroup=opacity[0].Group.GetClone();
            a.EventGroups.Add(flagGroup);a.EventGroups.Add(fadeGroup);
            a.Events.Add(new TAE.Event(0,clip.Duration,0,0,fallFlag.ToArray(),false){Group=flagGroup});
            foreach(var e in sourceFades)a.Events.Add(new TAE.Event(e.StartTime,Math.Min(e.EndTime,clip.Duration),193,0,[..e.GetParameterBytes(false),0,0,0,0,0,0,0,0],false){Group=fadeGroup});
            falling.Add(new(id,8100,clip.FrameCount,clip.Duration,plan.Count));
        }
        Need(falling.Any(m=>m.NativeAnimation==FallAnimation),"native graph's 7005 binding absent");
        animation.Files.Single(f=>f.ID==3000000).Bytes=nativeTae.Write();
        files["chr/"+target+".anibnd.dcx"]=animation.Write(DCX.Type.DCX_DFLT_10000_44_9);
        return new(files,skin,new("c3531",target,"c3530",skin.Vertices,skin.Triangles,159,falling,fades,physics.Report,hashes,[
            "Caller must clone owned passive Npc/Think883001, use native graph force-animation7005, position from source body sever sockets, disable combat/gravity on the controller, and clean up after the source fade (2.6 seconds).",
            "Source8100's five-second timing and opacity are preserved. Source FX13541/13534 are omitted because their FXR dependencies are not converted.",
            "Source c3531 has no source PWV; native typed unowned metadata is explicit. This is an export fixture, not proof of falling/contact/fade playback in game."
        ]));
    }
}
