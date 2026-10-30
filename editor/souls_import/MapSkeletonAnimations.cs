// SPDX-License-Identifier: GPL-2.0-or-later
using SoulsFormats;
using System.Text;

// Clone-only native playback slots; source translations retain the scattering
// bones. Native combat graphs and ordinary light-hit/death behavior are retained.
internal static class MapSkeletonAnimations {
    internal sealed record Wake(int SourceEntity,int SourceTrigger,int Entity,int Trigger,float Radius,float Delay,int EventID,float Duration);
    static readonly int[] WakeSlots=[7004,7005];
    static readonly int[] WakeVariants=[1,2,3];
    static string ClipName(int variant,int slot)=>$"a{variant:D3}_{slot:D6}";
    static void AddWakeClips(BND4 bank,string target) {
        // The donor's resident effect selects a001. Its project and character
        // already declare all four wake variants, but only a000 has a clip;
        // the other project paths point to the unavailable common c9999 bank.
        var projectFile=bank.Files.Single(f=>f.ID==7000300);var bytes=projectFile.Bytes.ToArray();
        var project=new NativeAnimation.Pack(bytes);var names=project.Array(project.Object("hkbProjectStringData")+16,8,16384);
        foreach(int slot in WakeSlots)foreach(int variant in WakeVariants) {
            string name=ClipName(variant,slot),shared=@"..\..\..\..\Model\chr\c9999\hkx\"+name+".hkx";
            var pointers=Enumerable.Range(0,names.Count).Select(i=>names.At+i*8).Where(at=>project.String(at)==shared).ToArray();
            if(pointers.Length!=1)throw new InvalidDataException("Native shared wake animation reference changed: "+name);
            string local=shared.Replace("c9999",target,StringComparison.Ordinal);
            if(local.Length!=shared.Length)throw new InvalidDataException("Wake animation path must preserve native fixups");
            Encoding.ASCII.GetBytes(local).CopyTo(bytes,project.Data+project.Pointer(pointers[0]));
            int id=checked(1000000000+variant*1000000+slot);
            if(bank.Files.Any(f=>f.ID==id))throw new InvalidDataException("Native wake variant is already occupied: "+id);
            var baseFile=bank.Files.Single(f=>f.ID==1000000000+slot);var file=baseFile.Clone();
            string oldName=ClipName(0,slot)+".hkx";
            if(!file.Name.EndsWith(oldName,StringComparison.Ordinal))throw new InvalidDataException("Native wake clip filename changed");
            file.ID=id;file.Name=file.Name[..^oldName.Length]+name+".hkx";
            int next=bank.Files.FindIndex(f=>f.ID>id);if(next<0)bank.Files.Add(file);else bank.Files.Insert(next,file);
        }
        projectFile.Bytes=bytes;
    }
    static void AddWakeEvents(TAE tae) {
        foreach(int slot in WakeSlots) {
            var original=tae.Animations.Single(a=>a.ID==slot);
            foreach(int variant in WakeVariants) {
                int id=variant*1000000+slot;
                if(tae.Animations.Any(a=>a.ID==id))throw new InvalidDataException("Native wake event variant is already occupied: "+id);
                var copy=new TAE.Animation(id,original.MiniHeader.GetClone(),ClipName(variant,slot)+".hkt");
                var groups=original.EventGroups.ToDictionary(g=>g,g=>g.GetClone());copy.EventGroups.AddRange(original.EventGroups.Select(g=>groups[g]));
                foreach(var e in original.Events) {
                    var clone=e.GetClone(false);clone.Group=e.Group is null?null:groups[e.Group];copy.Events.Add(clone);
                }
                int next=tae.Animations.FindIndex(a=>a.ID>id);if(next<0)tae.Animations.Add(copy);else tae.Animations.Insert(next,copy);
            }
        }
    }
    internal static void Apply(Func<string,byte[]?> read,string sourceModel,string target,SkinBake skin,Dictionary<string,byte[]> files) {
        string path="chr/"+target+".anibnd.dcx";var bank=BND4.Read(files[path]);
        AddWakeClips(bank,target);
        var rig=bank.Files.Single(f=>f.ID==1000000).Bytes.ToArray();
        var slots=new Dictionary<int,int>{{7004,9001},{7005,9061},{2014,2014},{2020,2020},{2040,2040}};
        var sourceEvents=BND3.Read(read("chr/"+sourceModel+".anibnd.dcx")??throw new FileNotFoundException("Source skeleton event bank unavailable")).Files
            .Where(f=>TAE.Is(f.Bytes.ToArray())).Select(f=>TAE.Read(f.Bytes)).SelectMany(t=>t.Animations).ToArray();
        int Jump(TAE.Event e){var bytes=e.GetParameterBytes(false);return e.Type==0&&bytes.Length>=4?BitConverter.ToInt32(bytes,0):-1;}
        // Firelink's source AI never selects the ordinary rig's protected spin
        // attacks. Its basic attacks and every giant attack lack this gate.
        int[] attacks=sourceModel=="c2900"?[3000,3001,3002,3003,3004,3005,3008]:[3000,3001,3002,3003,3004,3005,3006];
        if(attacks.Any(id=>sourceEvents.Single(a=>a.ID==id).Events.Any(e=>Jump(e)==24)))
            throw new InvalidDataException("Source skeleton attack protection changed");
        var protection=new Dictionary<int,TAE.Event[]>();
        foreach(var (nativeSlot,sourceID) in slots) {
            int[] ids=nativeSlot==7004?[8,39]:nativeSlot==7005?[8,24,49]:[24];
            var gates=sourceEvents.Single(a=>a.ID==sourceID).Events.Where(e=>ids.Contains(Jump(e))).DistinctBy(e=>(Jump(e),e.StartTime,e.EndTime)).ToArray();
            if(gates.Length!=ids.Length||gates.Select(Jump).Distinct().Count()!=ids.Length||gates.Any(e=>(e.GetParameterBytes(false).Length is <12 or >32||!e.GetParameterBytes(false).AsSpan(4,8).SequenceEqual(new byte[]{0,0,0,0,255,255,255,255})||e.GetParameterBytes(false).Skip(12).Take(4).Any(b=>b!=0))||e.StartTime<0||e.EndTime<=e.StartTime))
                throw new InvalidDataException("Source skeleton protection gates changed: "+sourceModel+"/"+sourceID);
            protection.Add(nativeSlot,gates);
        }
        var converted=new Dictionary<int,(byte[] Bytes,float Duration)>();var durations=new Dictionary<long,(float Old,float New)>();
        foreach(var file in bank.Files.Where(f=>f.ID>=1000000000)) {
            long id=file.ID-1000000000;int slot=(int)(id%1000000);
            if(!slots.TryGetValue(slot,out int sourceID))continue;
            if(!converted.TryGetValue(sourceID,out var result)) {
                var source=SourceAnimation.Load(read,sourceModel,sourceID);
                result=(NativeAnimation.Convert(source,rig,skin.Bones,skin.ProportionScale),source.Duration);converted.Add(sourceID,result);
            }
            var native=new NativeAnimation.Pack(file.Bytes.ToArray());float old=native.Float(native.Object("hkaSplineCompressedAnimation")+16);
            if(old<=0)throw new InvalidDataException("Invalid native skeleton playback duration");
            durations.Add(id,(old,result.Duration));file.Bytes=result.Bytes;
        }
        if(converted.Count!=slots.Count||!durations.ContainsKey(7004)||!durations.ContainsKey(7005))throw new InvalidDataException("Native skeleton playback slots changed");
        foreach(var f in bank.Files.Where(f=>TAE.Is(f.Bytes.ToArray()))) {
            var tae=TAE.Read(f.Bytes);
            AddWakeEvents(tae);
            foreach(var a in tae.Animations) {
                // The donor's torch attacks otherwise absorb a heavy hit while
                // dealing HP damage. Remove only these mismatched armor gates.
                if(a.ID is 1003013 or 1003014 or 1003015 or 1003016)a.Events.RemoveAll(e=>Jump(e)==24);
                if(!durations.TryGetValue(a.ID,out var time))continue;
                foreach(var e in a.Events) {
                    e.StartTime=Math.Clamp(e.StartTime/time.Old,0,1)*time.New;e.EndTime=Math.Clamp(e.EndTime/time.Old,0,1)*time.New;
                    if(Jump(e) is 23 or 78 or 79 or 86){e.StartTime=Math.Max(0,time.New-.1f);e.EndTime=time.New;}
                }
                if(protection.TryGetValue((int)(a.ID%1000000),out var gates)) {
                    var prototype=a.Events.First(e=>e.Type==0);var jumps=gates.Select(Jump).ToHashSet();
                    a.Events.RemoveAll(e=>jumps.Contains(Jump(e)));
                    foreach(var gate in gates) {
                        // DS1 stores the first twelve bytes; BB adds four zero
                        // default fields to the same jump-table command.
                        var parameters=new byte[16];Array.Copy(gate.GetParameterBytes(false),parameters,12);
                        a.Events.Add(new TAE.Event(Math.Clamp(gate.StartTime,0,time.New),Math.Clamp(gate.EndTime,0,time.New),0,prototype.Unk04,parameters,false){Group=prototype.Group});
                    }
                }
            }
            f.Bytes=tae.Write();
        }
        files[path]=bank.Write(DCX.Type.DCX_DFLT_10000_44_9);
    }

    static byte[] Args(params object[] args) {
        using var stream=new MemoryStream();using var writer=new BinaryWriter(stream);
        foreach(var a in args) {
            int size=a is byte or sbyte?1:4;while(stream.Position%size!=0)writer.Write((byte)0);
            switch(a){case byte n:writer.Write(n);break;case sbyte n:writer.Write(n);break;case int n:writer.Write(n);break;case uint n:writer.Write(n);break;case float n:writer.Write(n);break;default:throw new ArgumentException("Unsupported event argument");}
        }
        while(stream.Position%4!=0)writer.Write((byte)0);return stream.ToArray();
    }
    internal static (byte[] Script,IReadOnlyList<Wake> Wakes) Events(Func<string,byte[]?> sourceRead,byte[] native,IReadOnlyList<MapEnemySpawns.Spawn> actors) {
        var skeletons=actors.Where(p=>p.SourceModel is "c2900" or "c2910").ToDictionary(p=>p.SourceEntityID);
        if(skeletons.Count==0)return(native,[]);
        var source=EMEVD.Read(sourceRead("event/m10_02_00_00.emevd.dcx")??throw new FileNotFoundException("Source cemetery events unavailable"));
        var input=source.Events.Single(e=>e.ID==0).Instructions.Where(i=>i.Bank==2000&&i.ID==0&&i.ArgData.Length>=8&&BitConverter.ToInt32(i.ArgData,4)==11025200).ToArray();
        if(input.Length!=skeletons.Count)throw new InvalidDataException("Source cemetery awakening population changed");
        var wakeDurations=skeletons.Values.Select(s=>s.SourceModel).Distinct().ToDictionary(m=>m,m=>SourceAnimation.Load(sourceRead,m,9061).Duration);
        var script=EMEVD.Read(native);var constructor=script.Events.Single(e=>e.ID==0);var wakes=new List<Wake>();
        var used=script.Events.Select(e=>e.ID).ToHashSet();
        foreach(var call in input) {
            var p=call.ArgData;if(p.Length!=24)throw new InvalidDataException("Source cemetery awakening signature changed");
            int slot=BitConverter.ToInt32(p,0),trigger=BitConverter.ToInt32(p,8),entity=BitConverter.ToInt32(p,12),id=12415900+slot;
            float radius=BitConverter.ToSingle(p,16),delay=BitConverter.ToSingle(p,20);
            if(slot is <0 or >99||!float.IsFinite(radius)||radius is <=0 or >30||!float.IsFinite(delay)||delay is <0 or >10||
                !skeletons.TryGetValue(entity,out var actor)||!skeletons.TryGetValue(trigger,out var target)||!used.Add(id))throw new InvalidDataException("Unsupported cemetery awakening binding or occupied native event ID");
            var e=new EMEVD.Event(id,EMEVD.Event.RestBehaviorType.Restart);
            void Add(int bank,int command,params object[] a)=>e.Instructions.Add(new EMEVD.Instruction(bank,command,Args(a)));
            // Original native event12205020 proves these dormant-loop/exit slots.
            // Complete-event reloads start standing, matching source11025200.
            Add(4,0,(sbyte)1,actor.EntityID,(byte)1);
            Add(1000,2,(byte)0,(byte)1,(sbyte)1);
            Add(1003,101,(byte)0,(byte)1,(byte)1,0);
            Add(2004,19,actor.EntityID,261091);
            Add(2003,18,actor.EntityID,7004,(byte)1,(byte)0,(byte)0);
            Add(3,3,(sbyte)0,(byte)1,10000,target.EntityID,radius);
            if(delay>0)Add(1001,0,delay);
            Add(4,0,(sbyte)2,actor.EntityID,(byte)1);
            Add(1000,2,(byte)0,(byte)1,(sbyte)2);
            // Match native12205020: keep its dormant brain active, then
            // restore combat before a non-blocking one-shot. A timed reset
            // here would cancel a legitimate hit reaction during awakening.
            Add(2004,19,actor.EntityID,261010);
            Add(2003,18,actor.EntityID,7005,(byte)0,(byte)0,(byte)0);
            Add(1000,4,(byte)0);
            Add(1014,0);
            Add(2004,19,actor.EntityID,261010);
            Add(1000,4,(byte)0);
            script.Events.Add(e);constructor.Instructions.Add(new EMEVD.Instruction(2000,0,Args(0,(uint)id,0u)));
            wakes.Add(new(entity,trigger,actor.EntityID,target.EntityID,radius,delay,id,wakeDurations[actor.SourceModel]));
        }
        if(wakes.Select(w=>w.Entity).Distinct().Count()!=skeletons.Count)throw new InvalidDataException("Repeated cemetery awakening target");
        return(script.Write(DCX.Type.DCX_DFLT_10000_44_9),wakes);
    }
}
