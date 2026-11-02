// SPDX-License-Identifier: GPL-2.0-or-later
using SoulsFormats;
using System.Numerics;
using System.Text;

// DS1R's placed o0500 corpses select static animation clips rather than their
// standing FLVER bind pose. Bake each selected clip before static conversion;
// keep the original model name for texture lookup and group by model + clip.
internal static class SourceCorpsePose {
    internal sealed record Placement(string Name,string Model,int InitAnimation,int Animation,int? SourceEvent);
    internal sealed record Plan(IReadOnlyList<Placement> Placements,IReadOnlyList<string> Limits);
    internal static bool IsCorpse(string profile,string model)=>profile=="DS1R"&&model is "o0500" or "o0502" or "o0504";
    internal static bool Supports(string profile,string model,int animation)=>profile=="DS1R"&&(model switch {
        "o0500"=>animation is 10 or 11 or 20 or 21 or 31 or 35 or 42 or 50 or 51 or 81 or 82 or 90 or 100,
        "o0502"=>animation is 10 or 41 or 42,
        "o0504"=>animation is 60 or 61 or 70 or 81,
        _=>false
    });

    // Object variants have their own authored geometry/materials; they are not
    // character modelDispMask loadouts. Only ordinary Object records participate.
    // DummyObject/DummyEnemy records must not be made visible by this adapter.
    internal static Plan Resolve(string profile,string map,MSB1? source,Func<string,byte[]?> read) {
        if(profile!="DS1R"||source==null)return new([],[]);
        var objects=source.Parts.Objects.Where(p=>IsCorpse(profile,p.ModelName??"")).ToArray();
        var selected=new List<Placement>();EMEVD? events=null;
        foreach(var part in objects) {
            int animation=part.InitAnimID;int? sourceEvent=null;
            if(animation==-1) {
                // Burg's three barrel-supported bodies are held in pose100 by
                // source event11010400 until the support breaks (then anim101).
                // Resolve the actual parameterized constructor call and verify
                // its initial branch instead of guessing a default floor pose.
                if(map!="m10_01_00_00"||part.ModelName!="o0500"||part.EntityID<0)throw new InvalidDataException("Corpse has no verified initial pose: "+part.Name);
                events??=EMEVD.Read(read("event/"+map+".emevd.dcx")??throw new FileNotFoundException("Source corpse event unavailable"));
                var constructor=events.Events.Single(e=>e.ID==0);var candidates=constructor.Instructions.Where(i=>i.Bank==2000&&i.ID==0&&i.ArgData.Length==16&&BitConverter.ToInt32(i.ArgData,4)==11010400&&BitConverter.ToInt32(i.ArgData,8)==part.EntityID).ToArray();
                var entry=events.Events.Single(e=>e.ID==11010400);
                if(candidates.Length!=1||entry.Instructions.Count!=9||entry.Instructions[5].Bank!=2003||entry.Instructions[5].ID!=18||
                    !entry.Instructions[5].ArgData.SequenceEqual(new byte[]{0,0,0,0,100,0,0,0,1,0,0,0})||
                    !entry.Parameters.Any(p=>p.InstructionIndex==5&&p.TargetStartByte==0&&p.SourceStartByte==0&&p.ByteCount==4)||
                    entry.Parameters.Any(p=>p.InstructionIndex==5&&p.TargetStartByte+p.ByteCount>4))throw new InvalidDataException("Source barrel corpse initializer changed: "+part.Name);
                animation=100;sourceEvent=11010400;
            }
            if(!Supports(profile,part.ModelName!,animation))throw new NotSupportedException($"No verified static corpse pose for {map}/{part.Name}: {part.ModelName} animation {animation}");
            selected.Add(new(part.Name,part.ModelName!,part.InitAnimID,animation,sourceEvent));
        }
        return new(selected,["Preserves each source corpse object's actual model, every authored mesh/material, placement groups and selected static pose; no character model or armor loadout is substituted.","Source dummy object/enemy records remain excluded. Barrel-supported corpses retain their initial source event11010400 pose100; support-breaking motion101, physics and source treasure-state changes are not converted."]);
    }

    internal static FLVER2 Load(Func<string,byte[]?> read,string profile,string model,int animation) {
        if(!Supports(profile,model,animation))throw new NotSupportedException($"No verified static corpse pose decoder for {profile} {model} animation {animation}");
        var bytes=read($"obj/{model}.objbnd.dcx")??read($"obj/{model}.objbnd")??throw new FileNotFoundException("Source corpse object archive unavailable");
        var archive=BND3.Read(bytes);
        var flver=FLVER2.Read(File(archive,model+".flver"));
        var animations=BND3.Read(File(archive,model+".anibnd"));
        var skeleton=new SourceCollision.Tag(File(animations,"Skeleton.HKX").ToArray());
        var clip=new SourceCollision.Tag(File(animations,$"a00_{animation:D4}.hkx").ToArray());
        return Bake(flver,skeleton,clip);
    }

    static Memory<byte> File(BND3 archive,string name) {
        var matches=archive.Files.Where(f=>f.Name.Replace('\\','/').Split('/').Last().Equals(name,StringComparison.OrdinalIgnoreCase)).ToArray();
        if(matches.Length!=1)throw new InvalidDataException("Missing or ambiguous source corpse asset: "+name);
        return matches[0].Bytes;
    }
    static float Float(SourceCollision.Tag tag,int at) {
        float value=BitConverter.UInt32BitsToSingle(tag.UInt(at));
        return float.IsFinite(value)&&Math.Abs(value)<1000000?value:throw new InvalidDataException("Invalid source corpse transform");
    }
    static Quaternion Rotation(SourceCollision.Tag tag,int at)=>Unit(new(Float(tag,at),Float(tag,at+4),Float(tag,at+8),Float(tag,at+12)));
    static Quaternion Unit(Quaternion q) {
        if(!float.IsFinite(q.LengthSquared())||Math.Abs(q.LengthSquared()-1)>.002f)throw new InvalidDataException("Invalid source corpse quaternion");
        return Quaternion.Normalize(q);
    }
    static string Text(SourceCollision.Tag tag,int at) {
        var item=tag.Reference(at);
        if(item.Count<1||item.Count>256||tag.Name(item.Type)!="char")throw new InvalidDataException("Invalid corpse bone name");
        return Encoding.UTF8.GetString(Enumerable.Range(item.At,item.Count).Select(tag.Byte).ToArray()).TrimEnd('\0');
    }
    static Matrix4x4[] World(Matrix4x4[] local,int[] parents) {
        var world=new Matrix4x4[local.Length];var state=new byte[local.Length];
        Matrix4x4 At(int i) {
            if(i<0||i>=local.Length)throw new InvalidDataException("Invalid source corpse parent");
            if(state[i]==2)return world[i];if(state[i]==1)throw new InvalidDataException("Cyclic source corpse skeleton");state[i]=1;
            int parent=parents[i];world[i]=local[i]*(parent==-1?Matrix4x4.Identity:At(parent));state[i]=2;return world[i];
        }
        for(int i=0;i<world.Length;++i)At(i);return world;
    }
    static bool Finite(Vector3 p)=>float.IsFinite(p.X)&&float.IsFinite(p.Y)&&float.IsFinite(p.Z)&&p.LengthSquared()<1000000;

    static FLVER2 Bake(FLVER2 source,SourceCollision.Tag skeleton,SourceCollision.Tag clip) {
        if(source.Nodes.Count is <1 or >256||source.Meshes.Sum(m=>(long)m.Vertices.Count) is <1 or >2000000)throw new InvalidDataException("Source corpse exceeds geometry budget");
        var sk=skeleton.Items.Single(i=>skeleton.Name(i.Type)=="hkaSkeleton");
        var bones=skeleton.Array(sk,"bones");var parents=skeleton.Array(sk,"parentIndices");var reference=skeleton.Array(sk,"referencePose");
        if(bones.Count is <1 or >256||parents.Count!=bones.Count||reference.Count!=bones.Count)throw new InvalidDataException("Source corpse skeleton arrays do not match");
        var names=new string[bones.Count];var hp=new int[bones.Count];var local=new Matrix4x4[bones.Count];
        var positions=new Vector3[bones.Count];var scales=new Vector3[bones.Count];var rotations=new Quaternion[bones.Count];
        var flverByName=source.Nodes.Select((n,i)=>(n.Name,i)).ToDictionary(p=>p.Name,p=>p.i,StringComparer.Ordinal);
        var hkxToFlver=new int[bones.Count];var flverToHkx=Enumerable.Repeat(-1,source.Nodes.Count).ToArray();
        for(int i=0;i<bones.Count;++i) {
            var bone=skeleton.Element(bones,i);names[i]=Text(skeleton,skeleton.Nested(bone.Type,bone.At,"name").At);
            if(!flverByName.TryGetValue(names[i],out hkxToFlver[i])||flverToHkx[hkxToFlver[i]]!=-1)throw new InvalidDataException("Corpse skeleton bone name does not match FLVER: "+names[i]);
            flverToHkx[hkxToFlver[i]]=i;hp[i]=unchecked((short)skeleton.UShort(skeleton.Element(parents,i).At));
            int at=skeleton.Element(reference,i).At;positions[i]=skeleton.Vector(at);rotations[i]=Rotation(skeleton,at+16);scales[i]=skeleton.Vector(at+32);
            local[i]=Matrix4x4.CreateScale(scales[i])*Matrix4x4.CreateFromQuaternion(rotations[i])*Matrix4x4.CreateTranslation(positions[i]);
        }
        var referenceWorld=World(local,hp);var bindWorld=SkinRetarget.World(source.Nodes);
        var anim=clip.Items.Single(i=>clip.Name(i.Type)=="hkaSplineCompressedAnimation");
        var binding=clip.Items.Single(i=>clip.Name(i.Type)=="hkaAnimationBinding");
        int Int(string name)=>clip.Int(clip.Nested(anim.Type,anim.At,name).At);
        var tracks=clip.Array(binding,"transformTrackToBoneIndices");var data=clip.Array(anim,"data");
        if(Int("numberOfTransformTracks")!=tracks.Count||tracks.Count!=bones.Count||Int("numberOfFloatTracks")!=0||
            Int("numFrames")!=2||Int("numBlocks")!=1||Int("endian")!=0||data.Count is <1 or >65536||
            clip.Byte(clip.Nested(binding.Type,binding.At,"blendHint").At)!=0)
            throw new NotSupportedException("Corpse clip is not a verified static, non-additive animation");
        var raw=Enumerable.Range(data.At,data.Count).Select(clip.Byte).ToArray();
        using var reader=new BinaryReader(new MemoryStream(raw,false));
        var masks=new (byte Quant,byte Position,byte Rotation,byte Scale)[tracks.Count];
        for(int i=0;i<masks.Length;++i) {
            masks[i]=(reader.ReadByte(),reader.ReadByte(),reader.ReadByte(),reader.ReadByte());
            var m=masks[i];
            if(((m.Position|m.Rotation|m.Scale)&0xf0)!=0||((m.Position|m.Scale)&8)!=0)
                throw new NotSupportedException("Source corpse contains animated spline tracks");
            if(((m.Quant>>2)&15)!=1)throw new NotSupportedException("Unsupported source corpse quaternion quantization");
        }
        void Align(int size){long next=(reader.BaseStream.Position+size-1)/size*size;if(next>raw.Length)throw new InvalidDataException("Truncated source corpse pose");reader.BaseStream.Position=next;}
        float Scalar(){float value=reader.ReadSingle();if(!float.IsFinite(value)||Math.Abs(value)>1000000)throw new InvalidDataException("Invalid corpse track value");return value;}
        var assigned=new bool[bones.Count];Align(4);
        for(int i=0;i<masks.Length;++i) {
            int bone=unchecked((short)clip.UShort(clip.Element(tracks,i).At));
            if(bone<0||bone>=bones.Count||assigned[bone])throw new InvalidDataException("Invalid or repeated corpse transform track binding");assigned[bone]=true;
            var m=masks[i];
            for(int axis=0;axis<3;++axis)if((m.Position&(1<<axis))!=0)positions[bone][axis]=Scalar();
            Align(4);
            if(m.Rotation!=0) {
                // Havok THREECOMP40: three 12-bit values in [-1/sqrt(2),1/sqrt(2)],
                // followed by omitted-component index and sign. The rounded
                // quantization step matches the source animation decompressor.
                ulong packed=0;for(int b=0;b<5;++b)packed|=(ulong)reader.ReadByte()<<(b*8);
                int omitted=(int)((packed>>36)&3);var q=new float[4];int component=0;
                for(int c=0;c<4;++c)if(c!=omitted)q[c]=((int)((packed>>(12*component++))&4095)-2047)*.000345436f;
                q[omitted]=MathF.Sqrt(Math.Max(0,1-q.Sum(v=>v*v)))*(((packed>>38)&1)!=0?-1:1);
                rotations[bone]=Unit(new(q[0],q[1],q[2],q[3]));
            }
            Align(4);
            for(int axis=0;axis<3;++axis)if((m.Scale&(1<<axis))!=0)scales[bone][axis]=Scalar();
            Align(4);
            local[bone]=Matrix4x4.CreateScale(scales[bone])*Matrix4x4.CreateFromQuaternion(rotations[bone])*Matrix4x4.CreateTranslation(positions[bone]);
        }
        Align(16);if(reader.BaseStream.Position!=raw.Length)throw new InvalidDataException("Unexpected trailing corpse animation data");
        var posedWorld=World(local,hp);var transforms=new Matrix4x4[source.Nodes.Count];var normals=new Matrix4x4[source.Nodes.Count];var ready=new bool[source.Nodes.Count];
        void Prepare(int bone) {
            if(bone<0||bone>=source.Nodes.Count||flverToHkx[bone]<0)throw new InvalidDataException("Weighted corpse bone has no matching animation transform");
            if(ready[bone])return;
            int hk=flverToHkx[bone];var bind=bindWorld[bone];var rest=referenceWorld[hk];
            // DSR's reference pelvis is 0.71 mm below its FLVER bind pose and
            // its upper-arm twist frames differ by 2.38 degrees. Keep the
            // actual FLVER inverse bind; the HKX rest pose is only a check.
            float axisTolerance=names[hk] is "LUpArmTwist" or "RUpArmTwist"?.05f:.002f;
            if(Vector3.Distance(bind.Translation,rest.Translation)>.002f||
                Vector3.Distance(Vector3.TransformNormal(Vector3.UnitX,bind),Vector3.TransformNormal(Vector3.UnitX,rest))>axisTolerance||
                Vector3.Distance(Vector3.TransformNormal(Vector3.UnitY,bind),Vector3.TransformNormal(Vector3.UnitY,rest))>axisTolerance||
                Vector3.Distance(Vector3.TransformNormal(Vector3.UnitZ,bind),Vector3.TransformNormal(Vector3.UnitZ,rest))>axisTolerance)
                throw new InvalidDataException("Corpse FLVER bind pose differs from its named HKX reference bone");
            if(!Matrix4x4.Invert(bind,out var inverse))throw new InvalidDataException("Singular corpse inverse bind");
            transforms[bone]=inverse*posedWorld[hk];
            if(!Matrix4x4.Invert(transforms[bone],out var normalInverse))throw new InvalidDataException("Singular posed corpse bone");
            normals[bone]=Matrix4x4.Transpose(normalInverse);ready[bone]=true;
        }
        foreach(var mesh in source.Meshes) {
            if(!mesh.UseBoneWeights)throw new NotSupportedException("Source corpse contains unsupported rigid mesh");
            foreach(var vertex in mesh.Vertices) {
                var position=Vector3.Zero;var normal=Vector3.Zero;var tangents=new Vector3[vertex.Tangents.Count];float sum=0;
                for(int w=0;w<4;++w) {
                    float weight=vertex.BoneWeights[w];if(!float.IsFinite(weight)||weight<0)throw new InvalidDataException("Invalid corpse skin weight");if(weight==0)continue;
                    int palette=vertex.BoneIndices[w];if(palette<0||palette>=mesh.BoneIndices.Count)throw new InvalidDataException("Invalid corpse mesh bone palette index");
                    int bone=mesh.BoneIndices[palette];Prepare(bone);
                    position+=Vector3.Transform(vertex.Position,transforms[bone])*weight;
                    normal+=Vector3.TransformNormal(vertex.Normal,normals[bone])*weight;
                    for(int j=0;j<tangents.Length;++j){var v=vertex.Tangents[j];tangents[j]+=Vector3.TransformNormal(new(v.X,v.Y,v.Z),transforms[bone])*weight;}
                    sum+=weight;
                }
                if(Math.Abs(sum-1)>.001f||!Finite(position)||!Finite(normal)||normal.LengthSquared()<1e-12f)throw new InvalidDataException("Invalid posed corpse vertex");
                vertex.Position=position/sum;vertex.Normal=Vector3.Normalize(normal);vertex.NormalW=0;vertex.BoneIndices=new();vertex.BoneWeights=new();
                for(int j=0;j<tangents.Length;++j) {
                    var tangent=tangents[j]-vertex.Normal*Vector3.Dot(vertex.Normal,tangents[j]);
                    if(!Finite(tangent)||tangent.LengthSquared()<1e-12f)throw new InvalidDataException("Invalid posed corpse tangent");
                    vertex.Tangents[j]=new(Vector3.Normalize(tangent),vertex.Tangents[j].W);
                }
            }
            mesh.UseBoneWeights=false;mesh.BoneIndices.Clear();mesh.NodeIndex=-1;
            if(mesh.BoundingBox!=null){mesh.BoundingBox.Min=mesh.Vertices.Select(v=>v.Position).Aggregate(Vector3.Min);mesh.BoundingBox.Max=mesh.Vertices.Select(v=>v.Position).Aggregate(Vector3.Max);}
        }
        source.Nodes.Clear();source.Dummies.Clear();
        source.Header.BoundingBoxMin=source.Meshes.SelectMany(m=>m.Vertices).Select(v=>v.Position).Aggregate(Vector3.Min);
        source.Header.BoundingBoxMax=source.Meshes.SelectMany(m=>m.Vertices).Select(v=>v.Position).Aggregate(Vector3.Max);
        return source;
    }
}
