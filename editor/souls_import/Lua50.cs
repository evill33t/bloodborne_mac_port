// SPDX-License-Identifier: GPL-3.0-or-later
using System.Text;

// The installed DS1R and Bloodborne AI chunks use the same Lua 5.0 ABI.
// Parse its real constant/prototype tables; never replace arbitrary byte runs.
// This adapter changes only explicitly owned globals and checked constants.
internal static class Lua50 {
    internal static readonly byte[] Header=Convert.FromHexString("1B4C756150010408040608090908B6099368E7F57D41");
    internal sealed class Function {
        internal byte[]? Source;
        internal uint Line;
        internal byte Upvalues,Parameters,Vararg,Stack;
        internal uint[] Lines=[],Code=[];
        internal (byte[]? Name,uint Start,uint End)[] Locals=[];
        internal byte[]?[] UpvalueNames=[];
        internal object?[] Constants=[];
        internal Function[] Functions=[];
    }
    static void Require(bool value,string message){if(!value)throw new InvalidDataException("Lua 5.0: "+message);}
    // Native GOAL_COMMON_If builds "OnIf_" + GetBattleGoalId() at runtime.
    // These exported callbacks must follow the relocated numeric goal ID;
    // ordinary private-helper prefixes cannot participate in that dispatch.
    internal static void RelocateIfCallbacks(Function root,Dictionary<string,string> names,IReadOnlyDictionary<int,int> goals,ReadOnlySpan<byte> nativeDispatcher) {
        var exports=Exports(root);
        var callbacks=goals.Where(g=>exports.ContainsKey("OnIf_"+g.Key)).ToArray();
        if(callbacks.Length==0)return;
        Require(Convert.ToHexStringLower(System.Security.Cryptography.SHA256.HashData(nativeDispatcher))==
            "d4f1eee368ca3200d1637bbf965faaa2b19af26cf6a743d95a6c14a08af29717","native If dispatcher changed");
        foreach(var goal in callbacks) {
            string source="OnIf_"+goal.Key,target="OnIf_"+goal.Value;
            Require(goal.Key>0&&goal.Value>0&&names.ContainsKey(source),"unowned If callback");
            Require(!names.Any(n=>n.Key!=source&&n.Value==target),"If callback relocation aliases another global");
            names[source]=target;
        }
    }
    static string Text(byte[] bytes)=>new UTF8Encoding(false,true).GetString(bytes);
    internal static Function Read(byte[] bytes) {
        Require(bytes.Length is >22 and <=16*1024*1024&&bytes.AsSpan(0,22).SequenceEqual(Header),"unsupported bytecode ABI or size");
        using var reader=new BinaryReader(new MemoryStream(bytes));reader.BaseStream.Position=22;
        int budget=0;
        int Count(int limit){uint n=reader.ReadUInt32();Require(n<=limit,"table budget exceeded");return (int)n;}
        byte[]? String() {
            ulong count=reader.ReadUInt64();Require(count<=1024*1024&&count<=(ulong)(reader.BaseStream.Length-reader.BaseStream.Position),"invalid string bounds");
            if(count==0)return null;var b=reader.ReadBytes((int)count);Require(b[^1]==0,"unterminated string");return b[..^1];
        }
        Function ReadFunction(int depth) {
            Require(depth<=32&&++budget<=4096,"prototype budget exceeded");
            var f=new Function{Source=String(),Line=reader.ReadUInt32(),Upvalues=reader.ReadByte(),Parameters=reader.ReadByte(),Vararg=reader.ReadByte(),Stack=reader.ReadByte()};
            Require(f.Stack is >0 and <=250&&f.Parameters<=f.Stack&&f.Upvalues<=250&&f.Vararg<=1,"invalid function registers");
            f.Lines=Enumerable.Range(0,Count(1000000)).Select(_=>reader.ReadUInt32()).ToArray();
            f.Locals=Enumerable.Range(0,Count(100000)).Select(_=>(String(),reader.ReadUInt32(),reader.ReadUInt32())).ToArray();
            f.UpvalueNames=Enumerable.Range(0,Count(250)).Select(_=>String()).ToArray();
            f.Constants=Enumerable.Range(0,Count(100000)).Select(_=>reader.ReadByte() switch {
                0=>(object?)null,1=>reader.ReadByte() is byte b&&b<=1?(object)(b==1):throw new InvalidDataException("Invalid Lua boolean"),
                3=>reader.ReadDouble(),4=>String()??throw new InvalidDataException("Null Lua string constant"),
                _=>throw new InvalidDataException("Unsupported Lua constant tag")}).ToArray();
            f.Functions=Enumerable.Range(0,Count(4096)).Select(_=>ReadFunction(depth+1)).ToArray();
            f.Code=Enumerable.Range(0,Count(1000000)).Select(_=>reader.ReadUInt32()).ToArray();
            Require(f.Lines.Length==0||f.Lines.Length==f.Code.Length,"line table disagrees with code");
            foreach(uint word in f.Code) {
                int op=(int)(word&63),bx=(int)(word>>6&262143);Require(op<=34,"unknown opcode");
                if(op is 1 or 5 or 7){Require(bx<f.Constants.Length,"constant reference out of bounds");if(op is 5 or 7)Require(f.Constants[bx] is byte[],"global name is not a string");}
                if(op==34)Require(bx<f.Functions.Length,"closure reference out of bounds");
            }
            return f;
        }
        try{var result=ReadFunction(0);Require(reader.BaseStream.Position==bytes.Length,"trailing chunk data");return result;}
        catch(EndOfStreamException e){throw new InvalidDataException("Truncated Lua 5.0 chunk",e);}
    }
    internal static byte[] Write(Function root) {
        using var stream=new MemoryStream();using var writer=new BinaryWriter(stream);writer.Write(Header);
        void String(byte[]? b){writer.Write(b==null?0UL:(ulong)b.Length+1);if(b!=null){writer.Write(b);writer.Write((byte)0);}}
        void Function(Function f) {
            String(f.Source);writer.Write(f.Line);writer.Write(f.Upvalues);writer.Write(f.Parameters);writer.Write(f.Vararg);writer.Write(f.Stack);
            writer.Write(f.Lines.Length);foreach(uint n in f.Lines)writer.Write(n);
            writer.Write(f.Locals.Length);foreach(var l in f.Locals){String(l.Name);writer.Write(l.Start);writer.Write(l.End);}
            writer.Write(f.UpvalueNames.Length);foreach(var n in f.UpvalueNames)String(n);
            writer.Write(f.Constants.Length);foreach(var c in f.Constants)switch(c) {
                case null:writer.Write((byte)0);break;
                case bool b:writer.Write((byte)1);writer.Write((byte)(b?1:0));break;
                case double d:Require(double.IsFinite(d),"nonfinite number");writer.Write((byte)3);writer.Write(d);break;
                case byte[] b:writer.Write((byte)4);String(b);break;
                default:throw new InvalidDataException("Invalid Lua constant");
            }
            writer.Write(f.Functions.Length);foreach(var f2 in f.Functions)Function(f2);
            writer.Write(f.Code.Length);foreach(uint word in f.Code)writer.Write(word);
        }
        Function(root);var bytes=stream.ToArray();_=Read(bytes);return bytes;
    }
    internal static IEnumerable<Function> All(Function f){yield return f;foreach(var child in f.Functions)foreach(var item in All(child))yield return item;}
    internal static IReadOnlySet<string> Globals(Function root,bool written=false)=>All(root).SelectMany(f=>f.Code.Where(w=>(w&63)==(written?7u:5u)).Select(w=>Text((byte[])f.Constants[w>>6&262143]!))).ToHashSet(StringComparer.Ordinal);
    internal static IReadOnlyDictionary<string,double> Literals(Function root) {
        var values=new Dictionary<string,double>(StringComparer.Ordinal);
        for(int pc=1;pc<root.Code.Length;pc++) {
            uint word=root.Code[pc],previous=root.Code[pc-1];
            if((word&63)==7&&(previous&63)==1&&(word>>24)==(previous>>24)&&root.Constants[previous>>6&262143] is double value)
                Require(values.TryAdd(Text((byte[])root.Constants[word>>6&262143]!),value),"repeated literal global");
        }
        return values;
    }
    internal sealed record Export(Function Function,IReadOnlyList<double> Captures);
    internal static IReadOnlyDictionary<string,Export> Exports(Function root) {
        var result=new Dictionary<string,Export>(StringComparer.Ordinal);var numbers=new Dictionary<int,double>();
        for(int pc=0;pc<root.Code.Length;pc++) {
            uint word=root.Code[pc];int op=(int)(word&63),register=(int)(word>>24),bx=(int)(word>>6&262143);
            if(op==1&&root.Constants[bx] is double number){numbers[register]=number;continue;}
            if(op!=34){if(op is not(7 or 27))numbers.Remove(register);continue;}
            var f=root.Functions[bx];int setAt=pc+1+f.Upvalues;if(setAt>=root.Code.Length)continue;uint set=root.Code[setAt];
            if((set&63)!=7||(set>>24)!=register)continue;
            var captures=new List<double>();
            for(int i=0;i<f.Upvalues;i++) {
                uint bind=root.Code[pc+1+i];
                Require((bind&63)==0&&numbers.TryGetValue((int)(bind>>15&511),out _),"export captures a nonliteral root value");
                captures.Add(numbers[(int)(bind>>15&511)]);
            }
            // Shipped common_func2 defines Approach_Act twice. Lua keeps the
            // final unconditional definition, as this extraction does.
            result[Text((byte[])root.Constants[set>>6&262143]!)]=new(f,captures);pc=setAt;
        }
        return result;
    }
    internal static void PrependFunctions(Function root,IReadOnlyDictionary<string,Export> functions) {
        Require(functions.Count is >0 and <=128&&root.Upvalues==0,"invalid helper population");
        var code=new List<uint>();var constants=root.Constants.ToList();var children=root.Functions.ToList();
        foreach(var (name,export) in functions.OrderBy(p=>p.Key,StringComparer.Ordinal)) {
            var helper=export.Function;Require(helper.Upvalues==export.Captures.Count&&helper.Upvalues<=1&&name.Length is >0 and <=256,"unsupported helper capture");
            // Register0 is scratch before the original root initializes locals.
            // Relative branches and all original closure upvalue instructions
            // retain their exact words and offsets within the original body.
            int register=helper.Upvalues==0?0:1;
            if(helper.Upvalues==1){code.Add(1u|checked((uint)constants.Count<<6));constants.Add(export.Captures[0]);}
            code.Add(34u|checked((uint)children.Count<<6)|((uint)register<<24));children.Add(helper);
            if(helper.Upvalues==1)code.Add(0); // MOVE binds root register0.
            code.Add(7u|checked((uint)constants.Count<<6)|((uint)register<<24));constants.Add(Encoding.UTF8.GetBytes(name));
            if(helper.Upvalues==1)code.Add(33); // CLOSE before original root reuses register0.
        }
        int prefix=code.Count;root.Code=[..code,..root.Code];root.Constants=constants.ToArray();root.Functions=children.ToArray();
        if(root.Lines.Length>0)root.Lines=[..Enumerable.Repeat(0u,prefix),..root.Lines];
        root.Locals=root.Locals.Select(l=>(l.Name,checked(l.Start+(uint)prefix),checked(l.End+(uint)prefix))).ToArray();
        root.Stack=Math.Max(root.Stack,(byte)2);
    }
    internal static byte[] Rewrite(byte[] bytes,IReadOnlyDictionary<string,string> names,IReadOnlyDictionary<string,double> values,IReadOnlyDictionary<double,double>? numbers=null) {
        var root=Read(bytes);var seenNames=new HashSet<string>(StringComparer.Ordinal);var seenValues=new HashSet<string>(StringComparer.Ordinal);var seenNumbers=new HashSet<double>();
        foreach(var f in All(root))for(int i=0;i<f.Constants.Length;i++) {
            if(f.Constants[i] is byte[] text) {
                // Diagnostic strings in the shipped source use Shift-JIS.
                // Owned symbols are ASCII; preserve every other string byte.
                if(text.Any(b=>b>127))continue;string key=Encoding.ASCII.GetString(text);
                if(values.TryGetValue(key,out double value)) {
                    Require(double.IsFinite(value),"invalid global value");
                    var uses=f.Code.Select((w,p)=>(w,p)).Where(x=>(x.w&63) is 1 or 5 or 7&&(x.w>>6&262143)==i).ToArray();
                    Require(uses.Length>0&&uses.All(x=>(x.w&63)==5),"numeric substitution must address only global reads");
                    foreach(var (word,pc) in uses)f.Code[pc]=(word&~63u)|1u;
                    f.Constants[i]=value;seenValues.Add(key);
                } else if(names.TryGetValue(key,out string? name)){Require(name.Length is >0 and <=256&&!name.Any(char.IsControl),"invalid owned symbol");f.Constants[i]=Encoding.UTF8.GetBytes(name);seenNames.Add(key);}
            } else if(f.Constants[i] is double d&&numbers?.TryGetValue(d,out double replacement)==true) {
                Require(double.IsFinite(replacement),"invalid numeric replacement");f.Constants[i]=replacement;seenNumbers.Add(d);
            }
        }
        Require(names.Keys.All(seenNames.Contains)&&values.Keys.All(seenValues.Contains)&&(numbers==null||numbers.Keys.All(seenNumbers.Contains)),"requested substitution absent from chunk");
        return Write(root);
    }
}
