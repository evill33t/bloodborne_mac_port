// SPDX-License-Identifier: GPL-2.0-or-later
using SoulsFormats;
using System.Security.Cryptography;

internal static class MapWorldStateTests {
    static void Check(bool ok,string message){if(!ok)throw new Exception(message);}
    static void Reject(Action action,string message){try{action();}catch(InvalidDataException){return;}throw new Exception(message);}
    static bool Same(EMEVD.Instruction a,EMEVD.Instruction b)=>a.Bank==b.Bank&&a.ID==b.ID&&a.Layer==b.Layer&&a.ArgData.SequenceEqual(b.ArgData);
    static int Flag(int source)=>source switch{11020350=>14002900,11010700=>14002901,11400200=>14002902,_=>throw new Exception("Unknown source flag")};

    // Execute only the small native opcode subset emitted by this controller.
    // This validates branch targets/waits/persistence without claiming gameplay.
    static bool Execute(IEnumerable<EMEVD.Instruction> instructions,Dictionary<int,bool> flags,Dictionary<int,bool> enabled) {
        var code=instructions.ToArray();var groups=new Dictionary<sbyte,bool>();
        bool On(int flag)=>flags.GetValueOrDefault(flag);
        for(int pc=0;pc<code.Length;pc++) {
            var i=code[pc];var b=i.ArgData;
            switch((i.Bank,i.ID)) {
                case (1003,1): Check(b[2]==0,"Only absolute mapped flags are allowed");if(On(BitConverter.ToInt32(b,4))==(b[1]!=0))pc+=b[0];break;
                case (1000,3):pc+=b[0];break;
                case (2003,2): Check(b[4]==1,"Controller resets a persistent flag");flags[BitConverter.ToInt32(b)]=true;break;
                case (2011,1):case (2012,1):enabled[BitConverter.ToInt32(b)]=b[4]!=0;break;
                case (3,0):Check(b[2]==0,"Wait refers to an unmapped flag");sbyte group=unchecked((sbyte)b[0]);groups[group]=groups.GetValueOrDefault(group,true)&&(On(BitConverter.ToInt32(b,4))==(b[1]!=0));break;
                case (0,0):Check(b[0]==0&&b[1]==1,"Unexpected wait signature");if(!groups.GetValueOrDefault(unchecked((sbyte)b[2])))return false;break;
                case (1000,4):return true;
                default:throw new Exception("Unexpected generated opcode "+i.Bank+"["+i.ID+"]");
            }
        }
        return true;
    }
    static void State(MapWorldState.Result result,Dictionary<int,bool> enabled,bool open)=>
        Check(result.Bindings.All(b=>enabled.TryGetValue(b.EntityID,out bool state)&&state==(open?!b.FreshEnabled:b.FreshEnabled)),"World state is not consistently "+(open?"open":"closed"));

    static void Main(string[] args) {
        string sourceRoot=args[0],nativeRoot=args[1];
        string[] inputs=[Path.Combine(sourceRoot,"map/MapStudio/m10_02_00_00.msb"),Path.Combine(sourceRoot,"event/m10_02_00_00.emevd.dcx"),
            Path.Combine(nativeRoot,"map/MapStudio/m24_01_00_01.msb.dcx"),Path.Combine(nativeRoot,"event/m24_01_00_00.emevd.dcx")];
        var hashes=inputs.ToDictionary(p=>p,p=>SHA256.HashData(File.ReadAllBytes(p)));
        var source=MSB1.Read(inputs[0]);var sourceEvents=File.ReadAllBytes(inputs[1]);var nativeBytes=File.ReadAllBytes(inputs[2]);
        var originalScript=EMEVD.Read(inputs[3]);
        Check(originalScript.Events.All(e=>e.ID!=12415900),"Native fixture already uses skeleton fixture ID");
        var retained=new EMEVD.Event(12415900,EMEVD.Event.RestBehaviorType.Restart);retained.Instructions.Add(new(1001,0,1.5f));originalScript.Events.Add(retained);
        var ctor=originalScript.Events.Single(e=>e.ID==0);ctor.Parameters.Add(new(0,0,0,4));
        originalScript.Events.SingleOrDefault(e=>e.ID==50)?.Parameters.Add(new(0,0,0,4));
        var composed=originalScript.Write();
        (MSBB Map,Dictionary<string,MSBB.Part.MapPiece> Targets,MapCollisionBindings.Chunk[] Chunks,MSBB.Part.Collision[] Slots) Fixture(bool water=false) {
            var map=MSBB.Read(nativeBytes);map.Parts.MapPieces.First().EntityID=2419200;
            var targets=new Dictionary<string,MSBB.Part.MapPiece>();
            foreach(var name in water?new[]{"o1480_0000","o1481_0000","m2022B2_0000"}:new[]{"o1480_0000","o1481_0000"}) {
                var p=(MSBB.Part.MapPiece)map.Parts.MapPieces.First().DeepCopy();p.Name="pool_"+name;p.EntityID=-1;map.Parts.MapPieces.Add(p);targets.Add(name,p);
            }
            var slots=new List<MSBB.Part.Collision>();
            foreach(int n in new[]{991,992}) {
                string model="h999"+n;var definition=(MSBB.Model.Collision)map.Models.Collisions.First().DeepCopy();definition.Name=model;map.Models.Collisions.Add(definition);
                var part=(MSBB.Part.Collision)map.Parts.Collisions.First().DeepCopy();part.Name="pool_collision_"+n;part.ModelName=model;part.EntityID=-1;map.Parts.Collisions.Add(part);slots.Add(part);
            }
            var extra=(MSBB.Part.Collision)slots[0].DeepCopy();extra.Name+="_duplicate";map.Parts.Collisions.Add(extra);
            return(map,targets,[new("h0005B2_0000",[],[]),new("h0105B2_0000",[],[])],slots.ToArray());
        }
        var f=Fixture();var result=MapWorldState.Apply(source,f.Map,f.Targets,f.Chunks,f.Slots,composed,Flag,sourceEvents);
        Check(result.Bindings.Count==5&&result.Bindings.Select(b=>b.EntityID).Distinct().Count()==5&&result.Bindings.All(b=>b.EntityID is >=2419201 and <=2419299),"Entities are not unique or ignored an occupied ID");
        Check(result.Bindings.Count(b=>b.SourceName=="h0005B2_0000")==2,"A duplicate collision record escaped state control");
        Check(result.Report.Limitations.Any(s=>s.Contains("water m2022B2 is omitted")),"Missing water visual was not reported");
        var script=EMEVD.Read(result.Script);var pre=script.Events.Single(e=>e.ID==50);int originalPre=originalScript.Events.SingleOrDefault(e=>e.ID==50)?.Instructions.Count??0;
        var init=pre.Instructions.Take(pre.Instructions.Count-originalPre).ToArray();var control=script.Events.Single(e=>e.ID==MapWorldState.EventID);
        foreach(var original in originalScript.Events) {
            var converted=script.Events.Single(e=>e.ID==original.ID);int shift=original.ID==0?1:original.ID==50?init.Length:0;
            Check(converted.RestBehavior==original.RestBehavior&&converted.Instructions.Skip(shift).Zip(original.Instructions).All(p=>Same(p.First,p.Second))&&converted.Instructions.Count==original.Instructions.Count+shift,"Composed event instructions changed: "+original.ID);
            Check(converted.Parameters.Count==original.Parameters.Count&&converted.Parameters.Zip(original.Parameters).All(p=>p.First.InstructionIndex==p.Second.InstructionIndex+shift&&p.First.TargetStartByte==p.Second.TargetStartByte&&p.First.SourceStartByte==p.Second.SourceStartByte&&p.First.ByteCount==p.Second.ByteCount),"Composed parameter bindings shifted incorrectly");
        }
        Check(script.StringData.SequenceEqual(originalScript.StringData)&&script.LinkedFileOffsets.SequenceEqual(originalScript.LinkedFileOffsets),"Linked native scripts changed");
        var flags=new Dictionary<int,bool>{{999,true}};var enabled=new Dictionary<int,bool>();Execute(init,flags,enabled);State(result,enabled,false);
        Check(!Execute(control.Instructions,flags,enabled),"Fresh controller should await bells");State(result,enabled,false);
        flags[Flag(11010700)]=true;Check(!Execute(control.Instructions,flags,enabled),"One bell opened the pool");State(result,enabled,false);
        flags[Flag(11400200)]=true;Check(Execute(control.Instructions,flags,enabled),"Both bells did not finish controller");State(result,enabled,true);Check(flags[Flag(11020350)]&&flags[999],"Completion/native flags not preserved");
        flags[Flag(11010700)]=false;flags[Flag(11400200)]=false;enabled.Clear();Execute(init,flags,enabled);State(result,enabled,true);Execute(control.Instructions,flags,enabled);State(result,enabled,true);
        var arrivedAfterBells=new Dictionary<int,bool>{{Flag(11010700),true},{Flag(11400200),true}};
        enabled.Clear();Execute(init,arrivedAfterBells,enabled);State(result,enabled,true);Check(arrivedAfterBells[Flag(11020350)],"Preconstructor did not restore both-bells progress before rendering");
        foreach(var b in result.Bindings.Where(b=>b.Kind=="Collision"))Check(f.Map.Parts.Collisions.Single(p=>p.Name==b.TargetName).DisableStart==!b.FreshEnabled,"Map startup collision does not match fresh state");
        var back=MSBB.Read(f.Map.Write());Check(result.Bindings.All(b=>back.Parts.GetEntries().Single(p=>p.Name==b.TargetName).EntityID==b.EntityID),"Gate entities failed native MSB roundtrip");
        var variant=MSBB.Read(f.Map.Write());variant.Parts.MapPieces.RemoveAll(p=>result.Bindings.Any(b=>b.Kind=="MapPiece"&&b.TargetName==p.Name));
        foreach(var p in variant.Parts.Collisions.Where(p=>result.Bindings.Any(b=>b.Kind=="Collision"&&b.TargetModel==p.ModelName))){p.Name+="_variant";p.EntityID=-1;p.DisableStart=false;}
        MapWorldState.Mirror(variant,result);
        Check(result.Bindings.Where(b=>b.Kind=="Collision").All(b=>variant.Parts.Collisions.Any(p=>p.EntityID==b.EntityID&&p.DisableStart==!b.FreshEnabled)),"Variant collision IDs/state not mirrored");
        var badVariant=MSBB.Read(f.Map.Write());badVariant.Parts.Collisions.RemoveAll(p=>p.ModelName==f.Slots[1].ModelName);var beforeVariant=badVariant.Write();Reject(()=>MapWorldState.Mirror(badVariant,result),"Missing variant body accepted");Check(beforeVariant.SequenceEqual(badVariant.Write()),"Failed variant validation mutated the map");
        var conflict=MSBB.Read(f.Map.Write());conflict.Parts.MapPieces.First().EntityID=result.Bindings.First(b=>b.Kind=="Collision").EntityID;Reject(()=>MapWorldState.Mirror(conflict,result),"Variant ID collision accepted");
        var w=Fixture(true);var water=MapWorldState.Apply(source,w.Map,w.Targets,w.Chunks,w.Slots,composed,Flag,sourceEvents);Check(water.Bindings.Any(b=>b.SourceName=="m2022B2_0000"&&b.FreshEnabled),"Available water visual is not controlled");
        var changed=EMEVD.Read(sourceEvents);changed.Events.Single(e=>e.ID==11020350).Instructions[3].ArgData[4]^=1;
        var untouched=Fixture();Reject(()=>MapWorldState.Apply(source,untouched.Map,untouched.Targets,untouched.Chunks,untouched.Slots,composed,Flag,changed.Write()),"Changed source flag contract accepted");Check(untouched.Targets.Values.All(p=>p.EntityID==-1),"Failed validation mutated destination");
        var occupied=EMEVD.Read(composed);occupied.Events.Add(new(MapWorldState.EventID));Reject(()=>MapWorldState.Apply(source,untouched.Map,untouched.Targets,untouched.Chunks,untouched.Slots,occupied.Write(),Flag,sourceEvents),"Occupied controller ID accepted");
        Reject(()=>MapWorldState.Apply(source,untouched.Map,untouched.Targets,untouched.Chunks,untouched.Slots,composed,_=>14002900,sourceEvents),"Aliased flags accepted");
        untouched.Targets.Remove("o1481_0000");Reject(()=>MapWorldState.Apply(source,untouched.Map,untouched.Targets,untouched.Chunks,untouched.Slots,composed,Flag,sourceEvents),"Missing pool visual accepted");
        var full=Fixture();for(int id=2419201;id<=2419299;id++){var p=(MSBB.Part.MapPiece)full.Map.Parts.MapPieces.First().DeepCopy();p.Name="reserved_"+id;p.EntityID=id;full.Map.Parts.MapPieces.Add(p);}
        Reject(()=>MapWorldState.Apply(source,full.Map,full.Targets,full.Chunks,full.Slots,composed,Flag,sourceEvents),"Entity range exhaustion accepted");
        foreach(var (path,hash) in hashes)Check(SHA256.HashData(File.ReadAllBytes(path)).SequenceEqual(hash),"Original asset changed: "+path);
        Console.WriteLine("MapWorldState passed: actual source/native contracts, fresh/one-bell/both-bells/reload states, composed event preservation, duplicate collision records, variant identities, optional water, rejection boundaries and unchanged original assets.");
    }
}
