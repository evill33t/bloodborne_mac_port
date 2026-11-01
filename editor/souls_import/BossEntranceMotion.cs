// SPDX-License-Identifier: GPL-3.0-or-later
using System.Numerics;
using System.Security.Cryptography;
using System.Text;
using SoulsFormats;

// A single checked entrance, not a general controller/root-motion translator.
// Preserve every compressed native/source pose byte; only the Taurus waiting
// and jump slots receive authored root motion and source movement-state flags.
internal static class BossEntranceMotion {
    internal sealed record Report(string SourceClip,string SHA256,int Samples,float Duration,Vector3 Displacement,
        int WaitingSlot,int JumpSlot,IReadOnlyList<int> MovementFlags,string Limitation);
    internal sealed record Result(byte[] Bytes,Report Report);
    sealed record Motion(float Duration,Vector4[] Samples);
    const string JumpHash="77da90629ac5c1e6f32af2dab2d0401046a550233fc7f4fcfbd576b8b8f65d63";
    const string WaitHash="b2ade276780d0b99e4ad9d4f70b300f2bda17fe1720d070ce89b77c1a5443b1c";
    const string DonorHash="255919be3ecd7ad9943034b0a9196c59c4f225a53d31af88fbacb35d3bfa81e9";
    const string FrameClass="hkaDefaultAnimatedReferenceFrame";
    static string Leaf(string p)=>p.Replace('\\','/').Split('/').Last();
    static string Hash(byte[] bytes)=>Convert.ToHexStringLower(SHA256.HashData(bytes));
    static void Require(bool condition,string reason){if(!condition)throw new InvalidDataException("Taurus entrance: "+reason);}

    static Motion Source(byte[] bytes,string hash,int count,float duration,Vector4 end) {
        Require(Hash(bytes)==hash,"source motion clip changed");
        var tag=new SourceCollision.Tag(bytes);var animation=tag.Items.Single(i=>tag.Name(i.Type)=="hkaSplineCompressedAnimation");
        float F(int at)=>BitConverter.UInt32BitsToSingle(tag.UInt(at));
        var frame=tag.Reference(tag.Nested(animation.Type,animation.At,"extractedMotion").At);
        Require(tag.Name(frame.Type)==FrameClass&&F(tag.Nested(animation.Type,animation.At,"duration").At)==duration&&
            F(tag.Nested(frame.Type,frame.At,"duration").At)==duration&&tag.Vector(tag.Nested(frame.Type,frame.At,"up").At)==Vector3.UnitY&&
            tag.Vector(tag.Nested(frame.Type,frame.At,"forward").At)==Vector3.UnitZ,"source reference frame/basis changed");
        var samples=tag.Array(frame,"referenceFrameSamples");Require(samples.Count==count,"source sample count changed");
        var values=Enumerable.Range(0,count).Select(i=>{int at=tag.Element(samples,i).At;return new Vector4(tag.Vector(at),F(at+12));}).ToArray();
        Require(values[0]==Vector4.Zero&&values[^1]==end&&values.All(v=>float.IsFinite(v.LengthSquared())&&v.Length()<32&&v.W==0),"invalid authored trajectory");
        return new(duration,values);
    }

    internal static Result Apply(Func<string,byte[]?> readSource,byte[] packaged,byte[] nativeDonor) {
        Require(Hash(nativeDonor)==DonorHash,"native c3130 donor changed");
        var source=BND3.Read(readSource("chr/c2250.anibnd.dcx")??throw new FileNotFoundException("Taurus source animation archive unavailable"));
        byte[] Clip(string name)=>source.Files.Single(f=>Leaf(f.Name).Equals(name,StringComparison.OrdinalIgnoreCase)).Bytes.ToArray();
        var jump=Source(Clip("a00_9061.hkx"),JumpHash,126,4.1666665f,new(.22899437f,-11.460223f,-9.9610405f,0));
        var wait=Source(Clip("a00_9001.hkx"),WaitHash,2,.033333335f,Vector4.Zero);
        Require(wait.Samples.All(v=>v==Vector4.Zero),"waiting animation must be stationary");
        var donor=BND4.Read(nativeDonor);var bank=BND4.Read(packaged);
        var skeleton=NativeAnimation.Skeleton(bank.Files.Single(f=>f.ID==1000000).Bytes.ToArray(),allowMirroredScale:true);
        Require(skeleton.Names.Length==211&&skeleton.Names.Skip(125).All(n=>n.StartsWith("DS1_",StringComparison.Ordinal)),"not the checked appended Taurus rig");
        var nativeWait=donor.Files.Single(f=>f.ID==1000007000).Bytes.ToArray();
        var waiting=bank.Files.Single(f=>f.ID==1000007000);var jumping=bank.Files.Single(f=>f.ID==1000007010);
        float waitDuration=new NativeAnimation.Pack(waiting.Bytes.ToArray()).Float(new NativeAnimation.Pack(waiting.Bytes.ToArray()).Object("hkaSplineCompressedAnimation")+16);
        var waitingBytes=Rewrite(waiting.Bytes.ToArray(),nativeWait,new(waitDuration,wait.Samples),111,3.6666667f);
        var jumpBytes=Rewrite(jumping.Bytes.ToArray(),nativeWait,jump,131,4.3333335f);
        var sourceTae=TAE.Read(source.Files.Single(f=>Leaf(f.Name).EndsWith(".tae",StringComparison.OrdinalIgnoreCase)).Bytes);
        var taeFile=bank.Files.Single(f=>f.ID==3000000);var tae=TAE.Read(taeFile.Bytes);
        Require(!tae.BigEndian&&tae.Format==TAE.TAEFormat.DS3,"unsupported native TAE format");
        (int ID,float End)[] waitFlags=[(27,1.2666667f),(7,1.2666667f),(8,1.2666667f),(19,1.2666667f),(39,1.2666667f),(51,1.2666667f)];
        (int ID,float End)[] jumpFlags=[(27,1.7333333f),(7,4.3333335f),(8,1.7333333f),(19,1.5666667f),(39,1.7333333f),(51,1.9f)];
        void State(int sourceID,int nativeID,(int ID,float End)[] expected,bool loop) {
            var original=sourceTae.Animations.Single(a=>a.ID==sourceID);var flags=original.Events.Where(e=>e.Type==0).ToArray();
            Require(flags.Length==expected.Length,"source movement-state count changed");
            for(int i=0;i<flags.Length;i++) {
                byte[] args=flags[i].GetParameterBytes(false);
                Require(args.Length==12&&BitConverter.ToInt32(args)==expected[i].ID&&BitConverter.ToInt32(args,4)==0&&BitConverter.ToInt32(args,8)==-1&&flags[i].StartTime==0&&flags[i].EndTime==expected[i].End,"source movement-state contract changed");
            }
            var target=tae.Animations.Single(a=>a.ID==nativeID);var group=new TAE.EventGroup(0);
            // BB's checked retail jump-table events add a zero fourth scalar.
            var nativeTemplate=target.Events.First(e=>e.Type==0&&e.GetParameterBytes(false).Length==16).GetParameterBytes(false);
            Require(BitConverter.ToInt32(nativeTemplate,4)==0&&BitConverter.ToInt32(nativeTemplate,8)==-1&&BitConverter.ToInt32(nativeTemplate,12)==0,"native jump-table parameter layout changed");
            target.Events.Clear();target.EventGroups.Clear();target.EventGroups.Add(group);
            foreach(var flag in expected) {
                byte[] args=nativeTemplate.ToArray();BitConverter.GetBytes(flag.ID).CopyTo(args,0);
                // Source no-turn extends beyond9061 itself. Keep its active
                // interval through the clip, with no target cleanup tail.
                target.Events.Add(new TAE.Event(0,loop?waitDuration:Math.Min(flag.End,jump.Duration),0,0,args,false){Group=group});
            }
        }
        State(9001,7000,waitFlags,true);State(9061,7010,jumpFlags,false);
        // All edits are staged in newly parsed records before publication.
        waiting.Bytes=waitingBytes;jumping.Bytes=jumpBytes;taeFile.Bytes=tae.Write();
        return new(bank.Write(DCX.Type.DCX_DFLT_10000_44_9),new("c2250/a00_9061.hkx",JumpHash,126,jump.Duration,
            new(jump.Samples[^1].X,jump.Samples[^1].Y,jump.Samples[^1].Z),7000,7010,jumpFlags.Select(f=>f.ID).ToArray(),
            "Only Taurus slots7000/7010 are adapted. Native/source compressed pose channels are unchanged; jump timing and extracted movement use the checked source trajectory. Source sound/SFX/attack events are not copied. Native collision/controller playback and landing require gameplay validation; this is not a general source root-motion or TAE port."));
    }

    // NativeRigAppend intentionally preserves existing opaque data. This
    // equally bounded editor adds one typed reference-frame object when the
    // retail7010 clip has none; original object/curve offsets remain unchanged.
    static byte[] Rewrite(byte[] input,byte[] template,Motion motion,int frames,float oldDuration) {
        var p=new NativeAnimation.Pack(input);var donor=new NativeAnimation.Pack(template);int animation=p.Object("hkaSplineCompressedAnimation"),binding=p.Object("hkaAnimationBinding");
        Require(p.Int(animation+20)==211&&p.Int(animation+56)==frames&&p.Int(animation+60)==1&&p.Int(animation+64)==256&&p.Float(animation+16)==oldDuration&&input[p.Data+binding+80]==0,"native intro dimensions/blend changed");
        int sections=64+BitConverter.ToUInt16(input,62),header=sections+128;
        int I(int at)=>BitConverter.ToInt32(input,at);int local=I(header+24),global=I(header+28),virt=I(header+32),end=I(header+36);
        Require(I(header+40)==end&&I(header+44)==end&&p.Data+end==input.Length,"unsupported native imports/exports/tail");
        var body=input.AsSpan(p.Data,local).ToArray().ToList();var pointers=new SortedDictionary<int,int>();var globals=new List<(int Field,int Section,int Target)>();
        for(int at=local;at+8<=global;at+=8)if(I(p.Data+at)!=-1)pointers.Add(I(p.Data+at),I(p.Data+at+4));
        for(int at=global;at+12<=virt;at+=12)if(I(p.Data+at)!=-1)globals.Add((I(p.Data+at),I(p.Data+at+4),I(p.Data+at+8)));
        void Int(int at,int value){Require(at>=0&&at<=body.Count-4,"edit outside body");var bytes=BitConverter.GetBytes(value);for(int i=0;i<4;i++)body[at+i]=bytes[i];}
        void Float(int at,float value)=>Int(at,BitConverter.SingleToInt32Bits(value));
        int Append(byte[] bytes){while(body.Count%16!=0)body.Add(0);int at=body.Count;body.AddRange(bytes);return at;}
        var names=Classes(input,sections);var templateNames=Classes(template,64+BitConverter.ToUInt16(template,62));
        int frame=-1;try{frame=p.Object(FrameClass);}catch(InvalidDataException){}
        bool addFrame=frame<0;
        if(addFrame){int original=donor.Object(FrameClass);frame=Append(template.AsSpan(donor.Data+original,80).ToArray());Require(!pointers.ContainsKey(animation+32)&&!globals.Any(g=>g.Field==animation+32),"unexpected existing extracted-motion pointer");globals.Add((animation+32,2,frame));}
        else Require(globals.Count(g=>g.Field==animation+32&&g.Section==2&&g.Target==frame)==1&&!pointers.ContainsKey(animation+32),"intro extracted-motion pointer disagrees with typed object");
        int sampleAt=Append(motion.Samples.SelectMany(v=>new[]{v.X,v.Y,v.Z,v.W}.SelectMany(BitConverter.GetBytes)).ToArray());
        pointers[frame+56]=sampleAt;Int(frame+64,motion.Samples.Length);Int(frame+68,motion.Samples.Length|int.MinValue);
        Float(frame+48,motion.Duration);Float(animation+16,motion.Duration);
        float delta=motion.Duration/(frames-1);Float(animation+72,255*delta);Float(animation+76,1/(255*delta));Float(animation+80,delta);
        while(body.Count%16!=0)body.Add(0);int newLocal=body.Count;
        foreach(var pair in pointers){body.AddRange(BitConverter.GetBytes(pair.Key));body.AddRange(BitConverter.GetBytes(pair.Value));}
        while(body.Count%16!=0)body.Add(255);int newGlobal=body.Count;
        foreach(var g in globals)body.AddRange(new[]{g.Field,g.Section,g.Target}.SelectMany(BitConverter.GetBytes));while(body.Count%16!=0)body.Add(255);int newVirtual=body.Count;
        var virtualBytes=input.AsSpan(p.Data+virt,end-virt).ToArray().ToList();
        if(addFrame) {
            while(virtualBytes.Count>=4&&BitConverter.ToInt32(virtualBytes.ToArray(),virtualBytes.Count-4)==-1)virtualBytes.RemoveRange(virtualBytes.Count-4,4);
            Require(virtualBytes.Count%12==0,"unsupported virtual-fixup padding");
            int className;
            if(names.Names.TryGetValue(FrameClass,out var existing))className=existing.Name;
            else {var type=templateNames.Names[FrameClass];var bytes=BitConverter.GetBytes(type.Signature).Concat(new byte[]{9}).Concat(Encoding.ASCII.GetBytes(FrameClass+'\0')).ToArray();className=names.Used+5;names.Bytes.RemoveRange(names.Used,names.Bytes.Count-names.Used);names.Bytes.AddRange(bytes);while(names.Bytes.Count%16!=0)names.Bytes.Add(255);}
            virtualBytes.AddRange(new[]{frame,0,className}.SelectMany(BitConverter.GetBytes));while(virtualBytes.Count%16!=0)virtualBytes.Add(255);
        }
        body.AddRange(virtualBytes);int newEnd=body.Count;
        int classStart=I(sections+20),classEnd=I(sections+36),shift=names.Bytes.Count-classEnd;
        Require(classStart+classEnd==p.Data&&I(sections+64+20)==p.Data&&Enumerable.Range(0,6).All(i=>I(sections+64+24+i*4)==0),"unsupported nonempty type section");
        var prefix=input.AsSpan(0,classStart).ToArray();
        for(int i=0;i<6;i++)BitConverter.GetBytes(names.Bytes.Count).CopyTo(prefix,sections+24+i*4);
        BitConverter.GetBytes(p.Data+shift).CopyTo(prefix,sections+64+20);BitConverter.GetBytes(p.Data+shift).CopyTo(prefix,header+20);
        int[] offsets=[newLocal,newGlobal,newVirtual,newEnd,newEnd,newEnd];for(int i=0;i<6;i++)BitConverter.GetBytes(offsets[i]).CopyTo(prefix,header+24+i*4);
        var output=prefix.Concat(names.Bytes).Concat(body).ToArray();var check=new NativeAnimation.Pack(output);
        Require(globals.Single(g=>g.Field==animation+32)==(animation+32,2,frame)&&check.Object(FrameClass)==frame&&check.Array(frame+56,16,256).Count==motion.Samples.Length&&check.Float(frame+48)==motion.Duration,"reference-frame readback failed");
        return output;
    }
    sealed record ClassTable(List<byte> Bytes,int Used,Dictionary<string,(int Name,uint Signature)> Names);
    static ClassTable Classes(byte[] bytes,int section) {
        int I(int at)=>BitConverter.ToInt32(bytes,at);int start=I(section+20),end=I(section+36),at=0;var result=new Dictionary<string,(int,uint)>(StringComparer.Ordinal);
        Require(end>0&&Enumerable.Range(0,6).All(i=>I(section+24+i*4)==end),"unsupported class section fixups");
        while(at<end&&bytes[start+at]!=255) {
            Require(at<=end-6&&bytes[start+at+4]==9,"invalid native class record");int finish=Array.IndexOf(bytes,(byte)0,start+at+5);
            Require(finish>=start+at+5&&finish<start+end,"invalid class text");string name=Encoding.ASCII.GetString(bytes,start+at+5,finish-start-at-5);
            Require(result.TryAdd(name,(at+5,BitConverter.ToUInt32(bytes,start+at))),"repeated native class name");at=finish-start+1;
        }
        Require(bytes.AsSpan(start+at,end-at).ToArray().All(b=>b==255),"unsupported class-section padding");
        return new(bytes.AsSpan(start,end).ToArray().ToList(),at,result);
    }
}
