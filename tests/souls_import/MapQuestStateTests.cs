// SPDX-License-Identifier: GPL-2.0-or-later
using SoulsFormats;
using System.Security.Cryptography;

internal static class MapQuestStateTests {
    static void Check(bool ok,string message){if(!ok)throw new Exception(message);}
    static void Reject<T>(Action action,string message) where T:Exception {try{action();}catch(T){return;}throw new Exception(message);}
    static bool Same(EMEVD.Instruction a,EMEVD.Instruction b)=>a.Bank==b.Bank&&a.ID==b.ID&&a.Layer==b.Layer&&a.ArgData.SequenceEqual(b.ArgData);
    static int F(int flag)=>MapQuestState.Flag(flag);
    static readonly (int First,int Last,int Fresh)[] Ranges=[(1140,1169,1140),(1190,1209,1202),(1460,1489,1460)];

    // Dynamic condition members remain live while an event waits. Restart clears
    // only that event's groups, allowing absent/present/absent cycles to be tested.
    sealed class Machine(IEnumerable<EMEVD.Instruction> instructions,Dictionary<int,bool> flags,Dictionary<int,bool>? actors=null) {
        readonly EMEVD.Instruction[] code=instructions.ToArray();
        readonly Dictionary<sbyte,List<Func<bool>>> groups=[];
        internal readonly Dictionary<int,bool> Actors=actors??[];
        internal readonly Dictionary<int,bool> Immortal=[];
        internal readonly HashSet<int> Writes=[];
        int pc;
        bool On(int flag)=>flags.GetValueOrDefault(flag);
        bool Group(sbyte group)=>groups.TryGetValue(group,out var members)&&(group<0?members.Any(f=>f()):members.All(f=>f()));
        internal bool Run() {
            for(int steps=0;pc<code.Length;steps++) {
                Check(steps<10000,"Emitted event did not reach a wait/end");var i=code[pc];var b=i.ArgData;
                switch((i.Bank,i.ID)) {
                    case (1003,1):Check(b[2]==0,"Unexpected nonabsolute flag");if(On(BitConverter.ToInt32(b,4))==(b[1]!=0))pc+=b[0];break;
                    case (1000,1):if(Group(unchecked((sbyte)b[2]))==(b[1]!=0))pc+=b[0];break;
                    case (1000,3):pc+=b[0];break;
                    case (2003,2):int flag=BitConverter.ToInt32(b);flags[flag]=b[4]!=0;Writes.Add(flag);break;
                    case (2004,5):Actors[BitConverter.ToInt32(b)]=b[4]!=0;break;
                    case (2004,12):Immortal[BitConverter.ToInt32(b)]=b[4]!=0;break;
                    case (3,0): {
                        Check(b[2]==0,"Condition refers to unmapped event flag");sbyte group=unchecked((sbyte)b[0]);int target=BitConverter.ToInt32(b,4);bool state=b[1]!=0;
                        if(!groups.TryGetValue(group,out var members))groups.Add(group,members=[]);members.Add(()=>On(target)==state);break;
                    }
                    case (0,0):Check(b[0]==0,"Unexpected main-group assignment");if(Group(unchecked((sbyte)b[2]))!=(b[1]!=0))return false;break;
                    case (1000,4):if(b[0]==0)return true;Check(b[0]==1,"Unexpected end type");pc=0;groups.Clear();continue;
                    default:throw new Exception("Unexpected generated opcode "+i.Bank+"["+i.ID+"]");
                }
                pc++;
            }
            return true;
        }
    }

    static void OriginalEvents(EMEVD before,EMEVD after,int preShift,int ctorAppend=0) {
        Check(before.Format==after.Format&&before.LinkedFileOffsets.SequenceEqual(after.LinkedFileOffsets)&&before.StringData.SequenceEqual(after.StringData),"Native script identity/links changed");
        foreach(var e in before.Events) {
            var a=after.Events.Single(x=>x.ID==e.ID);int shift=e.ID==50?preShift:0,extra=e.ID==0?ctorAppend:0;
            Check(e.RestBehavior==a.RestBehavior&&a.Instructions.Count==e.Instructions.Count+shift+extra&&a.Instructions.Skip(shift).Take(e.Instructions.Count).Zip(e.Instructions).All(p=>Same(p.First,p.Second)),"Original instructions changed for event "+e.ID);
            Check(a.Parameters.Count==e.Parameters.Count&&a.Parameters.Zip(e.Parameters).All(p=>p.First.InstructionIndex==p.Second.InstructionIndex+shift&&p.First.TargetStartByte==p.Second.TargetStartByte&&p.First.SourceStartByte==p.Second.SourceStartByte&&p.First.ByteCount==p.Second.ByteCount&&p.First.UnkID==p.Second.UnkID),"Native parameter binding changed");
        }
    }
    static void Main(string[] args) {
        string sourceRoot=args[0],nativeRoot=args[1];
        string[] inputs=[Path.Combine(sourceRoot,"event/common.emevd.dcx"),Path.Combine(sourceRoot,"event/m10_02_00_00.emevd.dcx"),Path.Combine(nativeRoot,"event/m24_01_00_00.emevd.dcx")];
        var protectedInputs=inputs.Append(Path.Combine(nativeRoot,"param/gameparam/gameparam.parambnd.dcx"))
            .Concat(new[]{"event","script/talk"}.SelectMany(folder=>Directory.EnumerateFiles(Path.Combine(nativeRoot,folder),"*.dcx"))).Distinct().ToArray();
        var hashes=protectedInputs.ToDictionary(p=>p,p=>SHA256.HashData(File.ReadAllBytes(p)));
        var common=File.ReadAllBytes(inputs[0]);var firelink=File.ReadAllBytes(inputs[1]);var original=EMEVD.Read(inputs[2]);
        var ctor=original.Events.Single(e=>e.ID==0);ctor.Parameters.Add(new(0,0,0,4));
        var pre=original.Events.SingleOrDefault(e=>e.ID==50);if(pre==null){pre=new(50);pre.Instructions.Add(new(2003,2,999,(byte)1));original.Events.Add(pre);}pre.Parameters.Add(new(0,0,0,4));
        var sentinel=new EMEVD.Event(12415900,EMEVD.Event.RestBehaviorType.Restart);sentinel.Instructions.Add(new(1001,0,2f));original.Events.Add(sentinel);
        var native=original.Write();
        Check(MapQuestState.Flags.Count==198&&MapQuestState.Flags.Values.Distinct().Count()==198&&!MapQuestState.Flags.Values.Contains(MapQuestState.InitialFlag),"Quest ownership aliases or population changed");
        Check(MapQuestState.InitialFlag==52100499&&MapQuestState.Flags.Values.All(v=>v is >=52100100 and <=52100298)&&!MapQuestState.Flags.Values.Contains(52100258),"Stable reservation range/retired gap changed");
        foreach(var (source,target) in new[]{(280,52100100),(1140,52100107),(1202,52100149),(1460,52100159),(11010700,52100171),(11020350,52100174),(11400200,52100182),(71300093,52100257),(1150,52100259),(1489,52100298)})Check(F(source)==target,"Stable mapping anchor renumbered: "+source);
        foreach(var (first,last,_) in Ranges)foreach(int flag in Enumerable.Range(first,last-first+1))_=F(flag);
        Reject<NotSupportedException>(()=>F(int.MaxValue),"Unknown source flag silently allocated");
        MapQuestState.VerifyReservations(nativeRoot);
        var initialized=MapQuestState.Initialize(native,common);var init=EMEVD.Read(initialized);int prefixLength=init.Events.Single(e=>e.ID==50).Instructions.Count-pre.Instructions.Count;
        var prefix=init.Events.Single(e=>e.ID==50).Instructions.Take(prefixLength).ToArray();OriginalEvents(original,init,prefixLength);
        Check(prefix.All(i=>i.Bank!=3),"Initializer pollutes native preconstructor condition groups");
        var flags=new Dictionary<int,bool>{{999,true},{F(11020350),true},{F(11010700),true},{F(11400200),true}};
        var fresh=new Machine(prefix,flags);Check(fresh.Run(),"Initializer blocked");
        Check(fresh.Writes.SetEquals(new[]{F(1140),F(1202),F(1460),MapQuestState.InitialFlag}),"Fresh initializer writes beyond three defaults/marker");
        foreach(var (first,last,baseline) in Ranges)Check(Enumerable.Range(first,last-first+1).Where(f=>flags.GetValueOrDefault(F(f))).SequenceEqual(new[]{baseline}),"Incorrect fresh NPC state");
        Check(flags[999]&&flags[F(11020350)]&&flags[F(11010700)]&&flags[F(11400200)],"Native/pool progression flags clobbered");
        var before=new Dictionary<int,bool>(flags);var repeated=new Machine(prefix,flags);repeated.Run();Check(repeated.Writes.Count==0&&before.OrderBy(p=>p.Key).SequenceEqual(flags.OrderBy(p=>p.Key)),"Marker did not prevent initialization reset");
        var markerOnly=new Dictionary<int,bool>{{MapQuestState.InitialFlag,true},{999,true}};new Machine(prefix,markerOnly).Run();Check(markerOnly.Count==2,"Marker save with empty ranges was reset");
        foreach(var (first,last,baseline) in Ranges)foreach(int selected in Enumerable.Range(first,last-first+1)) {
            var progressed=new Dictionary<int,bool>{{F(selected),true},{999,true},{F(11020350),true}};new Machine(prefix,progressed).Run();
            Check(Enumerable.Range(first,last-first+1).Where(f=>progressed.GetValueOrDefault(F(f))).SequenceEqual(new[]{selected}),"Nonfresh full quest range reset: "+selected);
            Check(progressed[999]&&progressed[F(11020350)],"Unrelated flags changed while retaining range");
        }
        foreach(int at in new[]{13,17,53}) {
            var changed=EMEVD.Read(common);changed.Events.Single(e=>e.ID==50).Instructions[at].ArgData[8]^=1;
            Reject<InvalidDataException>(()=>MapQuestState.Initialize(native,changed.Write()),"Source common range contract accepted a change at "+at);
            changed=EMEVD.Read(common);changed.Events.Single(e=>e.ID==50).Instructions[at+1].ArgData[0]^=1;
            Reject<InvalidDataException>(()=>MapQuestState.Initialize(native,changed.Write()),"Source common fresh-state assignment accepted a change at "+at);
        }
        var targets=new Dictionary<int,int>{{6080,2419701},{6270,2419700},{6060,2419702}};
        var availability=MapQuestState.Availability(initialized,targets,firelink);var final=EMEVD.Read(availability.Script);
        int visiblePrefix=final.Events.Single(e=>e.ID==50).Instructions.Count-init.Events.Single(e=>e.ID==50).Instructions.Count;
        OriginalEvents(init,final,visiblePrefix,2);
        Check(availability.Bindings.Count==2&&availability.Bindings.Single(b=>b.SourceEntity==6080).AbsentFlags.SequenceEqual(new[]{F(1192)})&&availability.Bindings.Single(b=>b.SourceEntity==6270).AbsentFlags.SequenceEqual(new[]{F(1462),F(1464)}),"NPC availability sources/flags changed");
        var initialVisibility=final.Events.Single(e=>e.ID==50).Instructions.Take(visiblePrefix).ToArray();var actors=new Dictionary<int,bool>();var initialActors=new Machine(initialVisibility,flags,actors);initialActors.Run();Check(new[]{6080,6270}.All(id=>actors.GetValueOrDefault(targets[id])),"Fresh Petrus/Crestfallen not visible");
        Check(initialActors.Immortal.Count==1&&initialActors.Immortal.GetValueOrDefault(targets[6060]),"Source Anastacia immortality is missing or affects another actor");
        Check(initialVisibility[0].Bank==2004&&initialVisibility[0].ID==12&&BitConverter.ToInt32(initialVisibility[0].ArgData)==targets[6060]&&initialVisibility[0].ArgData[4]==1,"Anastacia protection is not the first preconstructor instruction");
        foreach(var binding in availability.Bindings)foreach(int absent in binding.AbsentFlags) {
            var state=new Dictionary<int,bool>{{MapQuestState.InitialFlag,true},{999,true},{F(11020350),true}};var visibility=new Dictionary<int,bool>();
            var machine=new Machine(final.Events.Single(e=>e.ID==binding.EventID).Instructions,state,visibility);
            Check(!machine.Run()&&visibility[binding.TargetEntity],"Present actor did not await absence");
            state[absent]=true;Check(!machine.Run()&&!visibility[binding.TargetEntity],"Absent state did not hide actor");
            var preload=new Dictionary<int,bool>();new Machine(initialVisibility,state,preload).Run();Check(!preload[binding.TargetEntity],"Preconstructor did not restore hidden actor");
            state[absent]=false;Check(!machine.Run()&&visibility[binding.TargetEntity],"Cleared absence did not restore actor");
            state[absent]=true;Check(!machine.Run()&&!visibility[binding.TargetEntity],"Availability loop did not repeat");
            Check(machine.Writes.Count==0&&state[999]&&state[F(11020350)]&&state[MapQuestState.InitialFlag],"Visibility controller writes quest/native flags");
        }
        var crest=availability.Bindings.Single(b=>b.SourceEntity==6270);var both=new Dictionary<int,bool>{{F(1462),true},{F(1464),true}};var bothActor=new Dictionary<int,bool>();var bothMachine=new Machine(final.Events.Single(e=>e.ID==crest.EventID).Instructions,both,bothActor);bothMachine.Run();Check(!bothActor[crest.TargetEntity],"Both absent flags not hidden");both[F(1462)]=false;bothMachine.Run();Check(!bothActor[crest.TargetEntity],"One remaining absence flag did not keep actor hidden");both[F(1464)]=false;bothMachine.Run();Check(bothActor[crest.TargetEntity],"Actor did not return when all absent flags cleared");
        foreach(int at in new[]{104,146}){var changed=EMEVD.Read(firelink);changed.Events.Single(e=>e.ID==50).Instructions[at].ArgData[4]^=1;Reject<InvalidDataException>(()=>MapQuestState.Availability(initialized,targets,changed.Write()),"Changed NPC source selection accepted");}
        foreach(int at in new[]{0,4}){var changed=EMEVD.Read(firelink);changed.Events.Single(e=>e.ID==11020110).Instructions[0].ArgData[at]^=1;Reject<InvalidDataException>(()=>MapQuestState.Availability(initialized,targets,changed.Write()),"Changed source Anastacia entity/protection accepted");}
        var substituted=EMEVD.Read(firelink);substituted.Events.Single(e=>e.ID==11020110).Parameters.Add(new(0,0,0,4));Reject<InvalidDataException>(()=>MapQuestState.Availability(initialized,targets,substituted.Write()),"Parameterized source protection accepted");
        Reject<InvalidDataException>(()=>MapQuestState.Availability(initialized,new Dictionary<int,int>{{6080,2419701},{6270,2419700}},firelink),"Missing Anastacia actor accepted");
        Reject<InvalidDataException>(()=>MapQuestState.Availability(initialized,new Dictionary<int,int>{{6080,2419701}},firelink),"Missing initial actor accepted");
        var occupied=EMEVD.Read(initialized);occupied.Events.Add(new(12415810));Reject<InvalidDataException>(()=>MapQuestState.Availability(occupied.Write(),targets,firelink),"Occupied availability event accepted");
        // The native reservation guard must catch literals in each scanned asset
        // family, including unaligned values and the separately reserved marker.
        var fixture=Directory.CreateTempSubdirectory("bbmap-quest-reservation-");
        try {
        Directory.CreateDirectory(Path.Combine(fixture.FullName,"param/gameparam"));Directory.CreateDirectory(Path.Combine(fixture.FullName,"event"));Directory.CreateDirectory(Path.Combine(fixture.FullName,"script/talk"));
        void Corpus(string family,int reserved) {
            foreach(var p in Directory.EnumerateFiles(fixture.FullName,"*",SearchOption.AllDirectories))File.Delete(p);
            var empty=new BND4();empty.Write(Path.Combine(fixture.FullName,"param/gameparam/gameparam.parambnd.dcx"));
            byte[] bytes=new byte[9];BitConverter.GetBytes(reserved).CopyTo(bytes,1);
            if(family=="param"){empty.Files.Add(new(Binder.FileFlags.Flag1,0,"probe.param",bytes));empty.Write(Path.Combine(fixture.FullName,"param/gameparam/gameparam.parambnd.dcx"));}
            else if(family=="compressed-talk") {
                var talk=new BND4();talk.Files.Add(new(Binder.FileFlags.Flag1|Binder.FileFlags.Compressed,0,"t000000.esd",bytes));
                talk.Write(Path.Combine(fixture.FullName,"script/talk/probe.talkesdbnd.dcx"),DCX.Type.DCX_DFLT_10000_44_9);
            }
            else File.WriteAllBytes(Path.Combine(fixture.FullName,family=="event"?"event/probe.emevd.dcx":"script/talk/probe.talkesdbnd.dcx"),DCX.Compress(bytes,DCX.Type.DCX_DFLT_10000_44_9));
            Reject<InvalidDataException>(()=>MapQuestState.VerifyReservations(fixture.FullName),"Native reservation overlap accepted in "+family);
        }
        Corpus("param",F(280));Corpus("event",F(1489));Corpus("talk",MapQuestState.InitialFlag);
        Corpus("compressed-talk",F(71100026));
        } finally {fixture.Delete(true);}
        foreach(var (path,hash) in hashes)Check(hash.SequenceEqual(SHA256.HashData(File.ReadAllBytes(path))),"Original event asset changed: "+path);
        Console.WriteLine("MapQuestState passed: 198 stable reservations/native scan, fresh defaults, all80 nonfresh range states, marker persistence, composed event/parameter preservation, Petrus/Crestfallen absence/restore loops, source contract rejection and "+hashes.Count+" unchanged original input hashes.");
    }
}
