// SPDX-License-Identifier: GPL-2.0-or-later
using SoulsFormats;

// Source world-state selection, independently of source dialogue or AI. Only
// the verified Firelink pool swap is supported; mapped flags belong to the mod.
internal static class MapWorldState {
    internal const int EventID=12415800;
    internal record Binding(string SourceName,string Kind,string TargetName,string TargetModel,int EntityID,bool FreshEnabled);
    internal record StateReport(int SourceEvent,int NativeEvent,int CompletionFlag,int UpperBellFlag,int LowerBellFlag,
        IReadOnlyList<string> Limitations);
    internal record Result(byte[] Script,IReadOnlyList<Binding> Bindings,StateReport Report);

    static byte[] Args(params object[] args) {
        using var stream=new MemoryStream();using var writer=new BinaryWriter(stream);
        foreach(var a in args) {
            int size=a is byte or sbyte?1:4;while(stream.Position%size!=0)writer.Write((byte)0);
            switch(a){case byte n:writer.Write(n);break;case sbyte n:writer.Write(n);break;case int n:writer.Write(n);break;case uint n:writer.Write(n);break;default:throw new ArgumentException("Unsupported world-state argument");}
        }
        while(stream.Position%4!=0)writer.Write((byte)0);return stream.ToArray();
    }
    static EMEVD.Instruction Ins(int bank,int command,params object[] args)=>new(bank,command,Args(args));
    static void Require(bool ok,string detail){if(!ok)throw new InvalidDataException("Firelink world state: "+detail);}
    static bool Same(EMEVD.Instruction a,EMEVD.Instruction b)=>a.Bank==b.Bank&&a.ID==b.ID&&a.Layer==b.Layer&&a.ArgData.AsSpan().SequenceEqual(b.ArgData);

    static void VerifySource(MSB1 source,byte[] bytes) {
        var required=new[]{("o1480_0000","o1480",1021480), ("o1481_0000","o1481",1021481),
            ("h0005B2_0000","h0005B2",1023500),("h0105B2_0000","h0105B2",1023510),
            ("m2022B2_0000","m2022B2",1023501),("h7000B2_0000","h7000B2",1023502)};
        foreach(var (name,model,id) in required) {
            var matches=source.Parts.GetEntries().Where(p=>p.Name==name).ToArray();
            Require(matches.Length==1&&matches[0].ModelName==model&&matches[0].EntityID==id,"source part identity changed: "+name);
        }
        Require(source.Parts.Objects.Count(p=>p.Name is "o1480_0000" or "o1481_0000" && p.InitAnimID==30)==2,"source pool object contract changed");
        Require(source.Parts.Collisions.Count(p=>p.Name is "h0005B2_0000" or "h0105B2_0000" && p.HitFilterID==8)==2,"source solid collision filters changed");
        Require(source.Parts.Collisions.Single(p=>p.Name=="h7000B2_0000").HitFilterID==9,"source auxiliary collision filter changed");
        var script=EMEVD.Read(bytes);var events=script.Events.Where(e=>e.ID==11020350).ToArray();
        Require(events.Length==1,"source pool event unavailable or repeated");var e=events[0];
        var expected=new[]{
            Ins(1003,1,(byte)7,(byte)1,(byte)1,0),Ins(2005,3,1021481,(byte)0),Ins(2011,1,1023510,(byte)0),
            Ins(3,0,(sbyte)1,(byte)1,(byte)0,11010700),Ins(3,0,(sbyte)1,(byte)1,(byte)0,11400200),Ins(0,0,(sbyte)0,(byte)1,(sbyte)1),
            Ins(2005,3,1021481,(byte)1),Ins(2011,1,1023510,(byte)1),Ins(2005,3,1021480,(byte)0),Ins(2011,1,1023500,(byte)0),
            Ins(2012,1,1023501,(byte)0),Ins(2011,1,1023502,(byte)0)};
        Require(e.RestBehavior==EMEVD.Event.RestBehaviorType.Default&&e.Parameters.Count==0&&e.Instructions.Count==expected.Length&&e.Instructions.Zip(expected).All(pair=>Same(pair.First,pair.Second)),"source event11020350 contract changed");
        var constructors=script.Events.Where(e=>e.ID==0).ToArray();
        Require(constructors.Length==1&&constructors[0].Instructions.Count(i=>Same(i,Ins(2000,0,0,11020350u,0u)))==1,"source constructor does not launch the verified pool event");
    }

    static IEnumerable<EMEVD.Instruction> Toggle(IEnumerable<Binding> bindings,bool open)=>bindings.Select(b=>
        Ins(b.Kind=="Collision"?2011:2012,1,b.EntityID,(byte)((open?!b.FreshEnabled:b.FreshEnabled)?1:0)));
    static void FlagSkip(List<EMEVD.Instruction> code,int index,int target,int flag,bool state) {
        int count=target-index-1;Require(count is >=0 and <=255,"state branch exceeds native skip limit");
        code[index]=Ins(1003,1,(byte)count,(byte)(state?1:0),(byte)0,flag);
    }
    static void Prefix(EMEVD.Event e,IReadOnlyList<EMEVD.Instruction> code) {
        foreach(var p in e.Parameters){Require(p.InstructionIndex>=0&&p.InstructionIndex<e.Instructions.Count,"invalid existing event parameter");p.InstructionIndex+=code.Count;}
        e.Instructions.InsertRange(0,code);
    }

    internal static Result Apply(MSB1 source,MSBB destination,IReadOnlyDictionary<string,MSBB.Part.MapPiece> staticTargets,
        IReadOnlyList<MapCollisionBindings.Chunk> chunks,IReadOnlyList<MSBB.Part.Collision> slots,byte[] composedScript,Func<int,int> mapFlag,byte[] sourceScript) {
        VerifySource(source,sourceScript);
        Require(chunks.Count==slots.Count&&slots.Select(p=>p.ModelName).Distinct(StringComparer.Ordinal).Count()==slots.Count,"collision slot bindings are ambiguous");
        var script=EMEVD.Read(composedScript);
        Require(script.Format==EMEVD.Game.Bloodborne,"target event script is not Bloodborne");
        Require(script.Events.All(e=>e.ID!=EventID)&&!script.Events.SelectMany(e=>e.Instructions).Any(i=>i.Bank==2000&&i.ID==0&&i.ArgData.Length>=8&&BitConverter.ToUInt32(i.ArgData,4)==EventID),"native event ID is already occupied");
        Require(script.Events.Count(e=>e.ID==0)==1&&script.Events.Count(e=>e.ID==50)<=1,"target constructors are missing or repeated");
        int done=mapFlag(11020350),upper=mapFlag(11010700),lower=mapFlag(11400200);
        Require(new[]{done,upper,lower}.All(f=>f>0&&f!=EventID)&&new[]{done,upper,lower}.Distinct().Count()==3,"mapped progression flags are invalid or aliased");
        var requests=new List<(string Source,MSBB.Part Part,bool Fresh)>();
        foreach(var (name,fresh) in new[]{("o1480_0000",true),("o1481_0000",false)}) {
            Require(staticTargets.TryGetValue(name,out var p)&&destination.Parts.MapPieces.Contains(p),"converted pool geometry is unavailable: "+name);
            requests.Add((name,p!,fresh));
        }
        // The source water is optional here because its shader is not yet ported.
        // If a later conversion supplies it, it follows the same pool state.
        bool water=staticTargets.TryGetValue("m2022B2_0000",out var waterPart);
        if(water){Require(destination.Parts.MapPieces.Contains(waterPart!),"water target is outside the destination map");requests.Add(("m2022B2_0000",waterPart!,true));}
        foreach(var (name,fresh) in new[]{("h0005B2_0000",true),("h0105B2_0000",false)}) {
            var indices=Enumerable.Range(0,chunks.Count).Where(i=>chunks[i].SourceName==name).ToArray();Require(indices.Length>0,"source collision chunks unavailable: "+name);
            foreach(int i in indices) {
                Require(destination.Parts.Collisions.Contains(slots[i]),"collision slot is outside destination map");
                foreach(var col in destination.Parts.Collisions.Where(p=>p.ModelName==slots[i].ModelName))requests.Add((name,col,fresh));
            }
        }
        Require(requests.Select(r=>r.Part.Name).Distinct(StringComparer.Ordinal).Count()==requests.Count,"repeated world-state target");
        var used=destination.Parts.GetEntries().Select(p=>p.EntityID).Concat(destination.Regions.GetEntries().Select(r=>r.EntityID)).Concat(destination.Events.GetEntries().Select(e=>e.EntityID)).ToHashSet();
        var bindings=new List<Binding>();int next=2419200;
        foreach(var r in requests) {
            while(next<=2419299&&used.Contains(next))next++;Require(next<=2419299,"reserved entity ID range is exhausted");used.Add(next);
            bindings.Add(new(r.Source,r.Part is MSBB.Part.Collision?"Collision":"MapPiece",r.Part.Name,r.Part.ModelName,next++,r.Fresh));
        }
        var initial=new List<EMEVD.Instruction>{Ins(1003,1),Ins(1003,1),Ins(1003,1)};
        int opened=initial.Count;initial.AddRange(Toggle(bindings,true));initial.Add(Ins(2003,2,done,(byte)1));
        int jump=initial.Count;initial.Add(Ins(1000,3));int closed=initial.Count;initial.AddRange(Toggle(bindings,false));
        FlagSkip(initial,0,opened,done,true);FlagSkip(initial,1,closed,upper,false);FlagSkip(initial,2,closed,lower,false);
        initial[jump]=Ins(1000,3,checked((byte)(initial.Count-jump-1)));
        var control=new EMEVD.Event(EventID,EMEVD.Event.RestBehaviorType.Restart);var code=control.Instructions;code.Add(Ins(1003,1));code.AddRange(Toggle(bindings,false));
        code.Add(Ins(3,0,(sbyte)1,(byte)1,(byte)0,upper));code.Add(Ins(3,0,(sbyte)1,(byte)1,(byte)0,lower));code.Add(Ins(0,0,(sbyte)0,(byte)1,(sbyte)1));
        code.Add(Ins(2003,2,done,(byte)1));int openAt=code.Count;code.AddRange(Toggle(bindings,true));code.Add(Ins(1000,4,(byte)0));FlagSkip(code,0,openAt,done,true);
        var pre=script.Events.SingleOrDefault(e=>e.ID==50);if(pre==null){pre=new EMEVD.Event(50);script.Events.Add(pre);}Prefix(pre,initial);
        Prefix(script.Events.Single(e=>e.ID==0),[Ins(2000,0,0,(uint)EventID,0u)]);script.Events.Add(control);
        var bytes=script.Write(DCX.Type.DCX_DFLT_10000_44_9);
        // Apply identity changes only after every contract and event serialization succeeds.
        for(int i=0;i<requests.Count;i++){requests[i].Part.EntityID=bindings[i].EntityID;if(requests[i].Part is MSBB.Part.Collision c)c.DisableStart=!requests[i].Fresh;}
        return new(bytes,bindings,new(11020350,EventID,done,upper,lower,
            [water?"Source pool water follows the state switch; shader and water physics remain approximations.":"Source water m2022B2 is omitted; only the pool floor/wood visuals and player-solid collision state are restored.",
             "Auxiliary source collision h7000B2 (filter9) is not converted by this controller. Bell interactions, Frampt, transport, dialogue and quests require separate conversion."]));
    }

    // Story variants may retain native visuals, but share these imported collision
    // bodies. Require an unambiguous record pairing rather than duplicate entity IDs.
    internal static void Mirror(MSBB variant,Result result) {
        var planned=new List<(MSBB.Part Part,Binding Binding)>();
        foreach(var group in result.Bindings.Where(b=>b.Kind=="Collision").GroupBy(b=>b.TargetModel,StringComparer.Ordinal)) {
            var targets=variant.Parts.Collisions.Where(c=>c.ModelName==group.Key).OrderBy(c=>c.Name,StringComparer.Ordinal).ToArray();
            var bindings=group.OrderBy(b=>b.TargetName,StringComparer.Ordinal).ToArray();
            Require(targets.Length==bindings.Length,"variant collision multiplicity differs: "+group.Key);
            for(int i=0;i<targets.Length;i++)planned.Add((targets[i],bindings[i]));
        }
        foreach(var binding in result.Bindings.Where(b=>b.Kind=="MapPiece")) {
            var parts=variant.Parts.MapPieces.Where(p=>p.Name==binding.TargetName).ToArray();Require(parts.Length<=1,"repeated variant visual: "+binding.TargetName);
            if(parts.Length==1){Require(parts[0].ModelName==binding.TargetModel,"variant visual model differs");planned.Add((parts[0],binding));}
        }
        var selected=planned.Select(p=>p.Part).ToHashSet();var assigned=planned.Select(p=>p.Binding.EntityID).ToHashSet();
        Require(!variant.Parts.GetEntries().Where(p=>!selected.Contains(p)).Select(p=>p.EntityID).Concat(variant.Regions.GetEntries().Select(r=>r.EntityID)).Concat(variant.Events.GetEntries().Select(e=>e.EntityID)).Any(assigned.Contains),"variant entity ID collision");
        foreach(var (part,binding) in planned){part.EntityID=binding.EntityID;if(part is MSBB.Part.Collision col)col.DisableStart=!binding.FreshEnabled;}
    }
}
