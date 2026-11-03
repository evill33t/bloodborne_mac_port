// SPDX-License-Identifier: GPL-2.0-or-later
using SoulsFormats;
using System.Security.Cryptography;

internal static class MapItems {
    internal record Lot(int SourceID,int TargetID,int SourceCollectionFlag,int CollectionFlag,IReadOnlyList<MapItemPolicy.Conversion> Goods,int IgnoredEquipment);
}
internal static class MapSolaireTests {
    static void Check(bool value,string message){if(!value)throw new Exception(message);}
    static int F(int flag)=>MapNpcQuestState.Flag(flag);
    sealed class EventMachine(IEnumerable<EMEVD.Instruction> instructions,Dictionary<int,bool> flags,int collection=0) {
        readonly EMEVD.Instruction[] code=instructions.ToArray();readonly Dictionary<sbyte,List<Func<bool>>> groups=[];
        internal readonly Dictionary<int,bool> Actors=[];internal readonly List<int> Awards=[];internal readonly HashSet<int> Writes=[];int pc;
        bool On(int flag)=>flags.GetValueOrDefault(flag);
        bool Group(sbyte group)=>groups.TryGetValue(group,out var g)&&(group<0?g.Any(f=>f()):g.All(f=>f()));
        internal void Run() {
            for(int steps=0;pc<code.Length;steps++) {
                Check(steps<10000,"Event did not reach wait/end");var i=code[pc];var b=i.ArgData;
                switch((i.Bank,i.ID)) {
                    case (1003,1):if(On(BitConverter.ToInt32(b,4))==(b[1]!=0))pc+=b[0];break;
                    case (1000,1):if(Group(unchecked((sbyte)b[2]))==(b[1]!=0))pc+=b[0];break;
                    case (2003,2):int flag=BitConverter.ToInt32(b);flags[flag]=b[4]!=0;Writes.Add(flag);break;
                    case (2003,4):Awards.Add(BitConverter.ToInt32(b));Check(collection>0,"Unexpected reward");flags[collection]=true;break;
                    case (2004,5):Actors[BitConverter.ToInt32(b)]=b[4]!=0;break;
                    case (3,0): {
                        sbyte g=unchecked((sbyte)b[0]);int target=BitConverter.ToInt32(b,4);bool enabled=b[1]!=0;
                        if(g==0){if(On(target)!=enabled)return;break;}
                        if(!groups.TryGetValue(g,out var members))groups.Add(g,members=[]);members.Add(()=>On(target)==enabled);break;
                    }
                    case (0,0):if(Group(unchecked((sbyte)b[2]))!=(b[1]!=0))return;break;
                    case (1000,4):if(b[0]==0)return;pc=0;groups.Clear();continue;
                    default:throw new Exception("Unexpected event opcode "+i.Bank+":"+i.ID);
                }
                pc++;
            }
        }
    }
    sealed class Conversation(ESD graph,Dictionary<int,bool> flags) {
        readonly Dictionary<long,ESD.State> states=graph.StateGroups[1];long state;bool enter=true;
        internal bool Action,Ended,Open;internal int Answer,Line;internal int Prompts;
        double Evaluate(byte[] bytes) {
            var stack=new List<double>();double Pop(){double v=stack[^1];stack.RemoveAt(stack.Count-1);return v;}
            for(int i=0;i<bytes.Length;i++) {
                byte op=bytes[i];
                if(op<=0x7f)stack.Add(op-64);
                else if(op==0x82){stack.Add(BitConverter.ToInt32(bytes,i+1));i+=4;}
                else if(op is >=0x84 and <=0x8a) {
                    int n=op-0x84;var args=new double[n];for(int j=n-1;j>=0;j--)args[j]=Pop();int f=(int)Pop();
                    stack.Add(f switch{1=>1,3 or 19=>0,15=>flags.GetValueOrDefault((int)args[0])?1:0,21=>Open?1:0,22=>Answer,56=>Action?1:0,57=>Ended?1:0,_=>throw new Exception("Unexpected ESD function "+f)});
                } else if(op is 0x94 or 0x95 or 0x98 or 0x99){double b=Pop(),a=Pop();stack.Add(op switch{0x94=>a>b?1:0,0x95=>a==b?1:0,0x98=>a!=0&&b!=0?1:0,_=>a!=0||b!=0?1:0});}
                else if(op==0xa6){}
                else if(op==0xa1){Check(i==bytes.Length-1&&stack.Count==1,"Malformed ESD expression");return Pop();}
                else throw new Exception("Unexpected ESD byte "+op);
            }
            throw new Exception("Unterminated ESD expression");
        }
        internal void Run() {
            for(int steps=0;steps<1000;steps++) {
                var s=states[state];
                if(enter){foreach(var command in s.EntryCommands){int[] a=command.Arguments.Select(b=>(int)Evaluate(b)).ToArray();switch(command.CommandID){case 1:Line=a[0];Ended=false;break;case 11:flags[a[0]]=a[1]!=0;break;case 17:Check(a.SequenceEqual(new[]{2,MapTalk.SolairePrompt,3,4,2}),"Native confirmation shape");Open=true;Answer=0;Prompts++;break;}}enter=false;}
                var next=s.Conditions.FirstOrDefault(c=>Evaluate(c.Evaluator)!=0);if(next==null)return;state=next.TargetState!.Value;enter=true;
            }
            throw new Exception("Conversation did not wait");
        }
        internal void Interact(){Action=true;Run();Action=false;}
        internal void Finish(){Ended=true;Run();}
        internal void Choose(bool yes){Answer=yes?1:2;Open=false;Run();}
    }
    static void Main(string[] args) {
        string ds=args[0],nativeRoot=args[1];string[] inputs=[Path.Combine(ds,"event/common.emevd.dcx"),Path.Combine(ds,"event/m10_01_00_00.emevd.dcx")];
        var hashes=inputs.ToDictionary(p=>p,p=>SHA256.HashData(File.ReadAllBytes(p)));var common=File.ReadAllBytes(inputs[0]);var source=File.ReadAllBytes(inputs[1]);
        var native=new EMEVD(EMEVD.Game.Bloodborne);native.Events.Add(new(0));native.Events.Add(new(50));
        var initialized=EMEVD.Read(MapNpcQuestState.Initialize(native.Write(),common));var flags=new Dictionary<int,bool>{{123456,true},{F(1464),true}};
        var initCode=initialized.Events.Single(e=>e.ID==50).Instructions;new EventMachine(initCode,flags).Run();
        Check(flags[F(1000)]&&flags[F(1464)]&&!flags.GetValueOrDefault(F(1460))&&flags[123456],"Fresh expansion reset an existing NPC range or native state");
        foreach(var r in MapNpcQuestState.Ranges){int count=Enumerable.Range(r.First,r.Last-r.First+1).Count(f=>flags.GetValueOrDefault(F(f)));Check(count==1,"NPC state defaults are not mutually exclusive");}
        var repeat=new EventMachine(initCode,flags);repeat.Run();Check(repeat.Writes.Count==0,"Repeated expansion initialization writes saved state");
        Check(MapNpcQuestState.Ranges.Length==44&&MapNpcQuestState.Flags.Values.Distinct().Count()==MapNpcQuestState.Flags.Count,"Expansion ownership aliases");
        Check(F(6)==52411116&&F(11300000)==52411117&&F(11010181)==52411165&&
            F(71010035)==52411167&&F(71010040)==52411168&&F(71020067)==52411169,
            "Pinwheel/Griggs append-only ownership changed");
        MapQuestState.VerifyReservations(nativeRoot,MapNpcQuestState.Flags.Values.Append(MapNpcQuestState.InitialFlag));
        var lot=new MapItems.Lot(1000,90001000,50000000,52100750,[new(100,900100,1,"quest-item","")],0);
        var result=MapSolaire.Apply(initialized.Write(),source,common,4039710,lot);var script=EMEVD.Read(result.Script);
        var presence=new EventMachine(script.Events.Single(e=>e.ID==MapSolaire.PresenceEvent).Instructions,flags);presence.Run();Check(presence.Actors[4039710],"Fresh Solaire absent");
        flags[F(1000)]=false;flags[F(1008)]=true;presence.Run();Check(!presence.Actors[4039710],"Anor Londo departure still shows Burg actor");
        flags[F(1008)]=false;flags[F(1007)]=true;presence.Run();Check(presence.Actors[4039710],"Source return state does not restore Burg actor");flags[F(1007)]=false;flags[F(1000)]=true;
        string[] names=["introduction","first offer","first acceptance","first refusal","second acceptance","second refusal","cooperation advice","sun","repeat first offer","repeat second offer"];
        int[][] writes=[[71010050],[71010051],[71010058],[],[11010591],[],[71010056,11010590],[71010057,11010590],[],[]];
        var branches=names.Select((name,i)=>new MapTalk.Branch(name,0,0,100+i,1,writes[i],writes[i].Select(F).ToArray(),"")).ToArray();
        var graph=MapTalk.Build(6001,branches,F,new ESD(true,2));var talk=new Conversation(ESD.Read(graph.Write()),flags);
        talk.Interact();Check(talk.Line==100,"Wrong introduction");talk.Finish();talk.Interact();Check(talk.Line==101,"First offer skipped");talk.Finish();Check(talk.Prompts==1,"First choice absent");
        talk.Choose(false);Check(talk.Line==103&&flags[F(71010054)]&&!flags.GetValueOrDefault(F(11010591)),"First refusal grants gift");talk.Finish();
        talk.Interact();Check(talk.Line==108,"Declined offer cannot be retried");talk.Finish();talk.Choose(true);Check(talk.Line==102,"First acceptance not spoken");talk.Finish();talk.Choose(false);Check(talk.Line==105&&!flags.GetValueOrDefault(F(11010591)),"Second refusal grants gift");talk.Finish();
        talk.Interact();Check(talk.Line==109,"Second offer cannot be retried");talk.Finish();talk.Choose(true);Check(talk.Line==104&&!flags.GetValueOrDefault(F(11010591)),"Gift committed before speech completed");talk.Finish();
        var gift=new EventMachine(script.Events.Single(e=>e.ID==MapSolaire.GiftEvent).Instructions,flags,lot.CollectionFlag);gift.Run();
        Check(gift.Awards.SequenceEqual(new[]{lot.TargetID})&&flags[F(1001)]&&!flags[F(1000)]&&!flags[F(11010591)],"Gift does not advance source state once");
        talk.Interact();Check(talk.Line==106,"Gift advice unavailable after request is cleared");talk.Finish();talk.Interact();Check(talk.Line==107,"Sun repeat unavailable");talk.Finish();
        var saved=new Dictionary<int,bool>(flags);new EventMachine(initCode,saved).Run();saved[F(11010591)]=true;
        var afterReload=new EventMachine(script.Events.Single(e=>e.ID==MapSolaire.GiftEvent).Instructions,saved,lot.CollectionFlag);afterReload.Run();Check(afterReload.Awards.Count==0&&saved[F(1001)]&&saved[123456],"Repeated gift after simulated reload duplicates reward or clears unrelated progress");
        foreach(var p in hashes)Check(p.Value.SequenceEqual(SHA256.HashData(File.ReadAllBytes(p.Key))),"Original source asset changed");
        Console.WriteLine("PASS: packaged-source contracts,44 NPC initial ranges, existing state preservation, Solaire presence/departure/return, both refusal/retry paths, speech completion and one-time gift progression (synthetic ESD/EMEVD execution)");
    }
}
