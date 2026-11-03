// SPDX-License-Identifier: GPL-3.0-or-later
using System.Security.Cryptography;
using System.Text;
using System.Text.RegularExpressions;

// Player NPCs share c0000's controller. Private weapon/hold TAEs need both
// selector children and Character animation-name bindings; a TAE alone falls
// back to the last retail child. Append exact native clip leaves and fixups.
internal static class NativePlayerAnimationBindings {
    internal sealed record Bank(int Source,int Target,IReadOnlySet<int> OffsetTypes,IReadOnlySet<int>? Actions=null);
    internal sealed record Result(byte[] Graph,byte[] Character,int Selectors,int Clips,IReadOnlyList<string> AnimationNames);
    static void Need(bool b,string why){if(!b)throw new InvalidDataException("Native player animation binding: "+why);}
    internal sealed class Pack {
        internal sealed record Object(int At,int Section,int NameOffset,string Type);
        internal readonly byte[] Bytes;
        internal readonly int Data,Size,Header,GlobalAt,VirtualAt,End;
        internal readonly SortedDictionary<int,int> Local=[];
        internal readonly SortedDictionary<int,(int Section,int At)> Global=[];
        internal readonly List<Object> Objects=[];
        internal Pack(byte[] bytes) {
            Bytes=bytes;Need(bytes.Length is >=272 and <=64*1024*1024&&U(0)==0x57e0e057&&U(4)==0x10c0c010&&I(12)==11&&bytes[16]==8&&bytes[17]==1&&I(20)==3,"unsupported packfile header");
            int sections=checked(64+BitConverter.ToUInt16(bytes,62));Header=sections+128;
            int classes=I(sections+20);Data=I(Header+20);Size=I(Header+24);GlobalAt=I(Header+28);VirtualAt=I(Header+32);End=I(Header+36);
            Need(classes>=0&&classes<Data&&Data>=Header+48&&Size>=0&&GlobalAt>=Size&&VirtualAt>=GlobalAt&&End>=VirtualAt&&Data<=bytes.Length-End&&I(Header+40)==End&&I(Header+44)==End,"unsupported section/import/export layout");
            for(int at=Size;at+8<=GlobalAt;at+=8)if(I(Data+at)!=-1)Need(Local.TryAdd(I(Data+at),I(Data+at+4)),"repeated local fixup");
            for(int at=GlobalAt;at+12<=VirtualAt;at+=12)if(I(Data+at)!=-1)Need(Global.TryAdd(I(Data+at),(I(Data+at+4),I(Data+at+8))),"repeated global fixup");
            foreach(var (field,to) in Local)Need(field>=0&&field<=Size-8&&to>=0&&to<Size&&Zero(field)&&!Global.ContainsKey(field),"invalid local pointer");
            foreach(var (field,to) in Global)Need(field>=0&&field<=Size-8&&to.Section==2&&to.At>=0&&to.At<Size&&Zero(field),"external/invalid global pointer");
            for(int at=VirtualAt;at+12<=End;at+=12)if(I(Data+at)!=-1) {
                int obj=I(Data+at),section=I(Data+at+4),name=I(Data+at+8);Need(section==0&&obj>=0&&obj<Size&&name>=0&&classes+name<Data,"invalid virtual fixup");
                Objects.Add(new(obj,section,name,TextAt(classes+name)));
            }
            Need(Objects.Count is >0 and <=32768&&Objects.Select(o=>o.At).Distinct().Count()==Objects.Count,"invalid virtual object population");
        }
        uint U(int at){Need(at>=0&&at<=Bytes.Length-4,"truncated scalar");return BitConverter.ToUInt32(Bytes,at);}
        int I(int at)=>unchecked((int)U(at));
        bool Zero(int field)=>Bytes.AsSpan(Data+field,8).IndexOfAnyExcept((byte)0)<0;
        internal int Int(int at){Need(at>=0&&at<=Size-4,"data scalar out of bounds");return I(Data+at);}
        string TextAt(int at){Need(at>=0&&at<Bytes.Length,"string out of bounds");int end=Array.IndexOf(Bytes,(byte)0,at,Math.Min(512,Bytes.Length-at));Need(end>=at,"unterminated string");return new UTF8Encoding(false,true).GetString(Bytes,at,end-at);}
        internal string Text(int field)=>TextAt(Data+(Local.TryGetValue(field,out int at)?at:throw new InvalidDataException("Missing native player string")));
        internal int[] Children(int field) {
            int count=Int(field+8);Need(count is >0 and <=8192&&((uint)Int(field+12)&0x7fffffff)>=count&&Local.TryGetValue(field,out _),"invalid child array");
            int at=Local[field];Need((long)at+count*8<=Size,"child array crosses data section");
            return Enumerable.Range(0,count).Select(i=>Global.TryGetValue(at+i*8,out var p)?p.At:throw new InvalidDataException("Missing native player child reference")).ToArray();
        }
        internal string[] Names(int field) {
            int count=Int(field+8);Need(count is >0 and <=32768&&((uint)Int(field+12)&0x7fffffff)>=count&&Local.TryGetValue(field,out _),"invalid animation-name array");
            int at=Local[field];Need((long)at+count*8<=Size,"animation names cross data section");return Enumerable.Range(0,count).Select(i=>Text(at+i*8)).ToArray();
        }
    }
    sealed class Edit(Pack original) {
        readonly List<byte> body=original.Bytes.AsSpan(original.Data,original.Size).ToArray().ToList();
        readonly SortedDictionary<int,int> local=new(original.Local);
        readonly SortedDictionary<int,(int Section,int At)> global=new(original.Global);
        readonly List<Pack.Object> objects=[..original.Objects];
        int Append(byte[] bytes,int alignment=16){while(body.Count%alignment!=0)body.Add(0);int at=body.Count;body.AddRange(bytes);return at;}
        int Text(string value)=>Append(Encoding.UTF8.GetBytes(value+'\0'),2);
        void Int(int at,int value){Need(at>=0&&at<=body.Count-4,"invalid appended scalar");var bytes=BitConverter.GetBytes(value);for(int i=0;i<4;i++)body[at+i]=bytes[i];}
        internal int CloneClip(Pack.Object clip,string name,string animation) {
            Need(clip.Type=="hkbClipGenerator"&&clip.At<=original.Size-352&&BitConverter.ToInt16(original.Bytes,original.Data+clip.At+188)==-1&&original.Bytes[original.Data+clip.At+191]==0,"unsupported clip leaf ABI");
            int at=Append(original.Bytes.AsSpan(original.Data+clip.At,352).ToArray());
            foreach(var (field,to) in original.Local.Where(p=>p.Key>=clip.At&&p.Key<clip.At+352)) {
                int offset=field-clip.At;Need(offset is 56 or 136 or 144,"unverified clip local pointer");local.Add(at+offset,offset==56?Text(name):offset==144?Text(animation):to);
            }
            Need(local.ContainsKey(at+56)&&local.ContainsKey(at+144),"clip leaf lacks authored names");
            foreach(var (field,to) in original.Global.Where(p=>p.Key>=clip.At&&p.Key<clip.At+352)) {
                int offset=field-clip.At;Need(offset is 16 or 152,"unverified clip object link");global.Add(at+offset,to);
            }
            objects.Add(clip with{At=at});return at;
        }
        internal void Children(int field,IReadOnlyList<int> children) {
            int at=Append(new byte[checked(children.Count*8)]);local[field]=at;Int(field+8,children.Count);Int(field+12,children.Count|int.MinValue);
            for(int i=0;i<children.Count;i++)global.Add(at+i*8,(2,children[i]));
        }
        internal void Names(int field,IReadOnlyList<string> names) {
            int at=Append(new byte[checked(names.Count*8)]);local[field]=at;Int(field+8,names.Count);Int(field+12,names.Count|int.MinValue);
            for(int i=0;i<names.Count;i++)local.Add(at+i*8,Text(names[i]));
        }
        internal byte[] Finish() {
            void Align(){while(body.Count%16!=0)body.Add(255);}
            Align();int localAt=body.Count;foreach(var (field,to) in local){body.AddRange(BitConverter.GetBytes(field));body.AddRange(BitConverter.GetBytes(to));}
            Align();int globalAt=body.Count;foreach(var (field,to) in global){body.AddRange(BitConverter.GetBytes(field));body.AddRange(BitConverter.GetBytes(to.Section));body.AddRange(BitConverter.GetBytes(to.At));}
            Align();int virtualAt=body.Count;foreach(var o in objects){body.AddRange(BitConverter.GetBytes(o.At));body.AddRange(BitConverter.GetBytes(o.Section));body.AddRange(BitConverter.GetBytes(o.NameOffset));}
            Align();int end=body.Count;Need(end<=64*1024*1024,"appended graph exceeds budget");
            var output=original.Bytes.AsSpan(0,original.Data).ToArray().Concat(body).Concat(original.Bytes.AsSpan(original.Data+original.End).ToArray()).ToArray();
            int[] offsets=[localAt,globalAt,virtualAt,end,end,end];for(int i=0;i<offsets.Length;i++)BitConverter.GetBytes(offsets[i]).CopyTo(output,original.Header+24+i*4);
            _=new Pack(output);return output;
        }
    }
    internal static Result Append(byte[] graphBytes,byte[] characterBytes,IReadOnlyList<Bank> banks) {
        Need(Convert.ToHexStringLower(SHA256.HashData(graphBytes))=="2e6a3464612043d951cb5eb4e516445a838f48d13356d1e44332e54b4d91e2fe"&&Convert.ToHexStringLower(SHA256.HashData(characterBytes))=="86ee3cddf5f761436b618004b5755edd54f5f1c585273ca97f388da5d5800373","unverified player graph/character version");
        Need(banks.Count is >0 and <=8&&banks.All(b=>b.Source is >=0 and <=999&&b.Target is >=0 and <=999&&b.Source!=b.Target&&b.OffsetTypes.Count>0&&b.OffsetTypes.All(t=>t is 11 or 13 or 14))&&banks.Select(b=>b.Target).Distinct().Count()==banks.Count,"invalid private bank reservation");
        var graph=new Pack(graphBytes);var character=new Pack(characterBytes);var edit=new Edit(graph);var byAt=graph.Objects.ToDictionary(o=>o.At);
        var targetPrefixes=banks.Select(b=>$"a{b.Target:D3}_").ToArray();
        Need(!graph.Objects.Where(o=>o.Type=="hkbClipGenerator").Any(o=>targetPrefixes.Any(p=>graph.Text(o.At+144).StartsWith(p,StringComparison.Ordinal))),"private player bank already has selector bindings");
        var names=new HashSet<string>(StringComparer.Ordinal);int selectors=0,clips=0;var used=new HashSet<int>();
        foreach(var selector in graph.Objects.Where(o=>o.Type=="CustomManualSelectorGenerator")) {
            int type=graph.Int(selector.At+152);var choices=banks.Where(b=>b.OffsetTypes.Contains(type)).ToArray();if(choices.Length==0)continue;
            var children=graph.Children(selector.At+136);var added=new List<int>();
            foreach(int child in children) {
                Need(byAt.TryGetValue(child,out var leaf)&&leaf.Type=="hkbClipGenerator","custom selector child is not a native clip leaf");
                string animation=graph.Text(child+144);var match=Regex.Match(animation,@"\Aa(\d{3})_\d{6}\z");Need(match.Success,"noncanonical clip animation name");
                int bank=int.Parse(match.Groups[1].Value),action=int.Parse(animation.AsSpan(5,6));var replacements=choices.Where(b=>b.Source==bank&&(b.Actions==null||b.Actions.Contains(action))).ToArray();
                foreach(var replacement in replacements) {
                    Need(used.Add(child),"selected clip leaf has multiple owners");string target=$"a{replacement.Target:D3}"+animation[4..];string oldName=graph.Text(child+56);Need(oldName.Length>=11,"clip node name is truncated");
                    // Some retail node names contain a space after a000. The
                    // native lookup compares a canonical11-character prefix.
                    added.Add(edit.CloneClip(leaf!,target+oldName[11..],target));names.Add(target);clips++;
                }
            }
            if(added.Count==0)continue;
            // Keep original indexes and the same native no-match animation.
            // A distinct fallback leaf avoids sharing active clip state twice
            // within one Custom selector's child array.
            var fallback=byAt[children[^1]];string fallbackAnimation=graph.Text(fallback.At+144);
            int last=edit.CloneClip(fallback,graph.Text(fallback.At+56)+"_import_fallback_"+selector.At,fallbackAnimation);
            edit.Children(selector.At+136,[..children,..added,last]);selectors++;
        }
        Need(selectors>0&&clips>0&&banks.All(b=>names.Any(n=>n.StartsWith($"a{b.Target:D3}_",StringComparison.Ordinal))),"private bank has no matching native selectors");
        const int nameArray=2792;Need(character.Objects.Any(o=>o.At==880&&o.Type=="hkbCharacterStringData"),"native Character string-data ABI changed");
        var existing=character.Names(nameArray);Need(existing.Length==6426&&names.All(n=>!existing.Contains(n,StringComparer.Ordinal)),"private Character binding is already occupied");
        var characterEdit=new Edit(character);var ordered=names.Order(StringComparer.Ordinal).ToArray();characterEdit.Names(nameArray,[..existing,..ordered]);
        return new(edit.Finish(),characterEdit.Finish(),selectors,clips,ordered);
    }
}
