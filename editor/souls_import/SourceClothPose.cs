// SPDX-License-Identifier: GPL-2.0-or-later
// Source capsule cloth is settled offline; this does not emulate Havok runtime cloth.
using SoulsFormats;
using System.Numerics;
using System.Text;
using System.Security.Cryptography;

internal static class SourceClothPose {
 internal sealed record BodyPose(string Bone,bool Fixed,Matrix4x4 Before,Matrix4x4 After);
 internal sealed record Report(string SourcePhysicsSHA256,int Bodies,int DynamicBodies,int Constraints,int ContactPairs,int SettlingSteps,int ProjectionSteps,int ChangedVertices,float GravityEnergyBefore,float GravityEnergyAfter,float MaximumPivotError,float MaximumAngularError,float MaximumPenetration,IReadOnlyList<BodyPose> Poses,string RigidCapeAnchor,IReadOnlyList<string> CapeFixedAnchors,IReadOnlyList<int> CapeMeshes,string Limitation);
 // Mesh-scoped anchors leave source body/equipment weights untouched. A static
 // cape cannot retain separate moving arm frames without stretching its drape.
 internal sealed record Result(byte[] Model,Report Report,IReadOnlyDictionary<int,string> Anchors);
 internal static byte[]? ReadPhysics(Func<string,byte[]?> read,string profile,string model) {
  if(profile!="DS1R"||model!="c2560")return null;
  var archive=BND3.Read(read("chr/"+model+".chrbnd.dcx")??throw new FileNotFoundException("Source cloth character is unavailable"));
  return archive.Files.Single(f=>f.Name.Replace('\\','/').Split('/').Last()==model+"_c.hkx").Bytes.ToArray();
 }

 internal sealed class Body {
  internal string Name="";internal Vector3 Position,LocalCenter,Inertia,A,B;internal Quaternion Rotation;internal float InverseMass,Radius,Gravity;internal Matrix4x4 Initial;
  internal Matrix4x4 World=>InverseMass==0?Initial:Matrix4x4.CreateFromQuaternion(Rotation)*Matrix4x4.CreateTranslation(Position-Vector3.Transform(LocalCenter,Rotation));
  internal Vector3 Point(Vector3 p)=>InverseMass==0?Vector3.Transform(p,Initial):Position+Vector3.Transform(p-LocalCenter,Rotation);
  internal Vector3 InvInertia(Vector3 axis)=>InverseMass==0?Vector3.Zero:Vector3.Transform(Vector3.Transform(axis,Quaternion.Conjugate(Rotation))*Inertia,Rotation);
  internal void Rotate(Vector3 angle){float length=angle.Length();if(length<1e-12f)return;Rotation=Quaternion.Normalize(Quaternion.Concatenate(Rotation,Quaternion.CreateFromAxisAngle(angle/length,Math.Min(length,.15f))));}
 }
 internal sealed record Limit(bool Enabled,int AxisA,int AxisB,float Min,float Max);
 internal sealed record Joint(int A,int B,Matrix4x4 FrameA,Matrix4x4 FrameB,Vector3 PivotA,Vector3 PivotB,Limit? Twist,Limit? Cone,Limit? Plane);
 static float Clamp(float value,float lo,float hi)=>Math.Clamp(value,lo,hi);
 static Vector3 Unit(Vector3 v)=>v.LengthSquared()<1e-16f?Vector3.UnitX:Vector3.Normalize(v);
 static Vector3 Axis(Matrix4x4 m,int axis)=>axis switch{0=>new(m.M11,m.M12,m.M13),1=>new(m.M21,m.M22,m.M23),2=>new(m.M31,m.M32,m.M33),_=>throw new InvalidDataException("Invalid constraint axis")};
 internal static (List<Body> Bodies,List<Joint> Joints) Read(byte[] bytes) {
  if(bytes.Length is <32 or >16*1024*1024)throw new InvalidDataException("Source cloth physics exceeds budget");
  var t=new SourceCollision.Tag(bytes);float F(int at)=>BitConverter.Int32BitsToSingle(t.Int(at));float Half(int at)=>BitConverter.Int32BitsToSingle(t.UShort(at)<<16);
  // hkTransform stores three XYZ basis vectors plus translation XYZ. The W
  // lanes are padding and can contain non-affine values; never read them.
  Matrix4x4 Transform(int at){var x=t.Vector(at);var y=t.Vector(at+16);var z=t.Vector(at+32);var p=t.Vector(at+48);return new(x.X,x.Y,x.Z,0,y.X,y.Y,y.Z,0,z.X,z.Y,z.Z,0,p.X,p.Y,p.Z,1);}

  string Name(SourceCollision.Tag.Item item){var s=t.Reference(t.Nested(item.Type,item.At,"name","stringAndFlag").At);return Encoding.UTF8.GetString(Enumerable.Range(s.At,s.Count-1).Select(t.Byte).ToArray());}
  var items=t.Items.Where(i=>i.Count==1&&t.Name(i.Type)=="hkpRigidBody").ToArray();var indices=items.Select((b,i)=>(b.At,i)).ToDictionary(p=>p.At,p=>p.i);var bodies=new List<Body>();
  foreach(var b in items) {
   var initial=Transform(t.Nested(b.Type,b.At,"motion","motionState","transform").At);var shape=t.Reference(t.Nested(b.Type,b.At,"collidable","shape").At);
   if(t.Name(shape.Type)!="hkpCapsuleShape")throw new InvalidDataException("Only source capsule cloth is supported");
   if(!float.IsFinite(initial.GetDeterminant())||!Matrix4x4.Decompose(initial,out var scale,out var rotation,out _ )||!float.IsFinite(rotation.LengthSquared())||Vector3.Distance(scale,Vector3.One)>.001f)throw new InvalidDataException("Invalid source cloth transform");
   int swept=t.Nested(b.Type,b.At,"motion","motionState","sweptTransform").At;var center=t.Vector(swept+64);int inv=t.Nested(b.Type,b.At,"motion","inertiaAndMassInv").At;float mass=F(inv+12);byte motion=t.Byte(t.Nested(b.Type,b.At,"motion","type").At);
   if(motion is not(2 or 3 or 4)||(mass==0)!=(motion==4)||mass<0||!float.IsFinite(mass)||t.UInt(t.Nested(b.Type,b.At,"collidable","broadPhaseHandle","collisionFilterInfo").At)!=0)throw new InvalidDataException("Unsupported source cloth motion/filter");
   bodies.Add(new(){Name=Name(b),Initial=initial,LocalCenter=center,Position=Vector3.Transform(center,initial),Rotation=rotation,Inertia=t.Vector(inv),InverseMass=mass,Gravity=Half(t.Nested(b.Type,b.At,"motion","gravityFactor").At),A=t.Vector(t.Nested(shape.Type,shape.At,"vertexA").At),B=t.Vector(t.Nested(shape.Type,shape.At,"vertexB").At),Radius=F(t.Nested(shape.Type,shape.At,"radius").At)});
  }
  if(bodies.Count is <1 or >128||bodies.Select(b=>b.Name).Distinct(StringComparer.Ordinal).Count()!=bodies.Count||bodies.Any(b=>!float.IsFinite(b.Radius)||b.Radius is <=0 or >1||!float.IsFinite(b.Gravity)||b.Gravity is <0 or >2||!float.IsFinite(b.Inertia.LengthSquared())||!float.IsFinite(b.LocalCenter.LengthSquared())||!float.IsFinite(b.A.LengthSquared())||!float.IsFinite(b.B.LengthSquared())||b.InverseMass>0&&(b.Inertia.X<=0||b.Inertia.Y<=0||b.Inertia.Z<=0)))throw new InvalidDataException("Invalid source cloth body dimensions");
  var joints=new List<Joint>();foreach(var c in t.Items.Where(i=>i.Count==1&&t.Name(i.Type)=="hkpConstraintInstance")) {
   int at=t.Nested(c.Type,c.At,"entities").At;int a=indices[t.Reference(at).At],b=indices[t.Reference(at+8).At];var d=t.Reference(t.Nested(c.Type,c.At,"data").At);string type=t.Name(d.Type);
   if(type=="hkpBallAndSocketConstraintData") {joints.Add(new(a,b,Matrix4x4.Identity,Matrix4x4.Identity,t.Vector(t.Nested(d.Type,d.At,"atoms","pivots","translationA").At),t.Vector(t.Nested(d.Type,d.At,"atoms","pivots","translationB").At),null,null,null));continue;}
   if(type!="hkpRagdollConstraintData"||t.Byte(t.Nested(d.Type,d.At,"atoms","ragdollMotors","isEnabled").At)!=0)throw new InvalidDataException("Unsupported active source cloth motor/constraint");
   Limit L(string name,bool twist=false) {var x=t.Nested(d.Type,d.At,"atoms",name);int i(string key)=>t.Byte(t.Nested(x.Type,x.At,key).At);float f(string key)=>F(t.Nested(x.Type,x.At,key).At);var value=new Limit(i("isEnabled")!=0,i(twist?"twistAxis":"twistAxisInA"),i(twist?"refAxis":"refAxisInB"),f("minAngle"),f("maxAngle"));if(value.AxisA is <0 or >2||value.AxisB is <0 or >2||!float.IsFinite(value.Min)||!float.IsFinite(value.Max)||value.Min>value.Max||!twist&&i("angleMeasurementMode")!=(name=="coneLimit"?0:1))throw new InvalidDataException("Unsupported source cloth angular limit");return value;}
   var fa=Transform(t.Nested(d.Type,d.At,"atoms","transforms","transformA").At);var fb=Transform(t.Nested(d.Type,d.At,"atoms","transforms","transformB").At);joints.Add(new(a,b,fa,fb,fa.Translation,fb.Translation,L("twistLimit",true),L("coneLimit"),L("planesLimit")));
  }
  if(joints.Count is <1 or >256||joints.Any(j=>j.A==j.B))throw new InvalidDataException("Invalid source cloth constraint population");
  return(bodies,joints);
 }
 static void Linear(Body a,Body b,Vector3 pa,Vector3 pb,Vector3 axis,float error) {
  var ra=pa-a.Position;var rb=pb-b.Position;var ca=Vector3.Cross(ra,axis);var cb=Vector3.Cross(rb,axis);var ia=a.InvInertia(ca);var ib=b.InvInertia(cb);float inverse=a.InverseMass+b.InverseMass+Vector3.Dot(ca,ia)+Vector3.Dot(cb,ib);
  if(inverse<1e-12f)return;float lambda=-error/inverse;a.Position+=axis*(lambda*a.InverseMass);b.Position-=axis*(lambda*b.InverseMass);a.Rotate(ia*lambda);b.Rotate(-ib*lambda);
 }
 static void Angular(Body a,Body b,Vector3 axis,float rotateA) {
  var ia=a.InvInertia(axis);var ib=b.InvInertia(axis);float inverse=Vector3.Dot(axis,ia+ib);if(inverse<1e-12f)return;float lambda=rotateA/inverse;a.Rotate(ia*lambda);b.Rotate(-ib*lambda);
 }
 static float Signed(Vector3 a,Vector3 b,Vector3 axis)=>MathF.Atan2(Vector3.Dot(Vector3.Cross(a,b),axis),Clamp(Vector3.Dot(a,b),-1,1));
 static void Solve(List<Body> bs,Joint j) {
  var a=bs[j.A];var b=bs[j.B];foreach(var axis in new[]{Vector3.UnitX,Vector3.UnitY,Vector3.UnitZ}) {var pa=a.Point(j.PivotA);var pb=b.Point(j.PivotB);Linear(a,b,pa,pb,axis,Vector3.Dot(pa-pb,axis));}
  if(j.Cone is not{} cone)return;
  Matrix4x4 A()=>j.FrameA*a.World;Matrix4x4 B()=>j.FrameB*b.World;
  if(cone.Enabled){var u=Unit(Axis(A(),cone.AxisA));var v=Unit(Axis(B(),cone.AxisB));float angle=MathF.Acos(Clamp(Vector3.Dot(u,v),-1,1));if(angle>cone.Max)Angular(a,b,Unit(Vector3.Cross(u,v)),angle-cone.Max);}
  if(j.Plane is {Enabled:true} plane) {var u=Unit(Axis(A(),plane.AxisA));var v=Unit(Axis(B(),plane.AxisB));float angle=MathF.Asin(Clamp(Vector3.Dot(u,v),-1,1));float correction=Clamp(angle,plane.Min,plane.Max)-angle;if(Math.Abs(correction)>1e-7f)Angular(a,b,Unit(Vector3.Cross(u,v)),correction);}
  if(j.Twist is {Enabled:true} twist) {var ma=A();var mb=B();var axis=Unit(Axis(ma,twist.AxisA)+Axis(mb,twist.AxisA));var u=Axis(ma,twist.AxisB);var v=Axis(mb,twist.AxisB);u=Unit(u-axis*Vector3.Dot(axis,u));v=Unit(v-axis*Vector3.Dot(axis,v));float angle=Signed(u,v,axis),correction=angle-Clamp(angle,twist.Min,twist.Max);if(Math.Abs(correction)>1e-7f)Angular(a,b,axis,correction);}
 }
 static (Vector3 A,Vector3 B) Closest(Vector3 p1,Vector3 q1,Vector3 p2,Vector3 q2) {
  var d1=q1-p1;var d2=q2-p2;var r=p1-p2;float a=Vector3.Dot(d1,d1),e=Vector3.Dot(d2,d2),f=Vector3.Dot(d2,r),s,t;
  if(a<=1e-12f&&e<=1e-12f)return(p1,p2);
  if(a<=1e-12f){s=0;t=Clamp(f/e,0,1);}else{float c=Vector3.Dot(d1,r);if(e<=1e-12f){t=0;s=Clamp(-c/a,0,1);}else{float b=Vector3.Dot(d1,d2),den=a*e-b*b;s=den!=0?Clamp((b*f-c*e)/den,0,1):0;t=(b*s+f)/e;if(t<0){t=0;s=Clamp(-c/a,0,1);}else if(t>1){t=1;s=Clamp((b-c)/a,0,1);}}}
  return(p1+d1*s,p2+d2*t);
 }
 static float Contact(Body a,Body b,bool apply) {
  var (pa,pb)=Closest(a.Point(a.A),a.Point(a.B),b.Point(b.A),b.Point(b.B));var delta=pa-pb;float length=delta.Length(),penetration=a.Radius+b.Radius-length;if(penetration<=0)return 0;if(apply)Linear(a,b,pa,pb,Unit(delta),-penetration);return penetration;
 }
 internal static float AngularError(List<Body> bs,Joint j) {
  var ma=j.FrameA*bs[j.A].World;var mb=j.FrameB*bs[j.B].World;float worst=0;
  if(j.Cone is {Enabled:true} cone){float angle=MathF.Acos(Clamp(Vector3.Dot(Unit(Axis(ma,cone.AxisA)),Unit(Axis(mb,cone.AxisB))),-1,1));worst=Math.Max(worst,angle-Clamp(angle,cone.Min,cone.Max));}
  if(j.Plane is {Enabled:true} plane){float angle=MathF.Asin(Clamp(Vector3.Dot(Unit(Axis(ma,plane.AxisA)),Unit(Axis(mb,plane.AxisB))),-1,1));worst=Math.Max(worst,Math.Abs(angle-Clamp(angle,plane.Min,plane.Max)));}
  if(j.Twist is {Enabled:true} twist){var axis=Unit(Axis(ma,twist.AxisA)+Axis(mb,twist.AxisA));var u=Axis(ma,twist.AxisB);var v=Axis(mb,twist.AxisB);u=Unit(u-axis*Vector3.Dot(axis,u));v=Unit(v-axis*Vector3.Dot(axis,v));float angle=Signed(u,v,axis);worst=Math.Max(worst,Math.Abs(angle-Clamp(angle,twist.Min,twist.Max)));}
  return worst;
 }
 internal static Result Bake(byte[] sourceModel,byte[] sourcePhysics) {
  var model=FLVER2.Read(sourceModel);if(model.Nodes.Count is <1 or >256||model.Meshes.Sum(m=>(long)m.Vertices.Count)>1000000||model.Meshes.Any(m=>m.Dynamic!=1))throw new InvalidDataException("Unsupported source cloth model");
  foreach(var mesh in model.Meshes)foreach(var v in mesh.Vertices)if(!float.IsFinite(v.Position.LengthSquared())||!float.IsFinite(v.Normal.LengthSquared())||Enumerable.Range(0,4).Any(i=>!float.IsFinite(v.BoneWeights[i])||v.BoneWeights[i]<0)||Math.Abs(Enumerable.Range(0,4).Sum(i=>v.BoneWeights[i])-1)>.001f)throw new InvalidDataException("Invalid source cloth vertex/weights");
  var (bs,js)=Read(sourcePhysics);
  if(bs.Count!=57||js.Count!=119||!bs.Any(b=>b.Name=="Cloth_c07")||!model.Nodes.Any(n=>n.Name=="c2560_weapon"))throw new InvalidDataException("Source cloth adapter currently supports authored c2560 only");

  var related=js.Select(j=>(Math.Min(j.A,j.B),Math.Max(j.A,j.B))).ToHashSet();var contacts=new List<(int A,int B)>();for(int a=0;a<bs.Count;a++)for(int b=a+1;b<bs.Count;b++)if((bs[a].InverseMass>0||bs[b].InverseMass>0)&&!related.Contains((a,b)))contacts.Add((a,b));
  float Energy()=>bs.Where(b=>b.InverseMass>0).Sum(b=>b.Position.Y/b.InverseMass*9.80665f);float before=Energy();
  for(int step=0;step<1500;step++) {
   foreach(var b in bs)if(b.InverseMass>0)b.Position.Y-=9.80665f*b.Gravity/3600;
   for(int iteration=0;iteration<12;iteration++){foreach(var j in js)Solve(bs,j);foreach(var (a,b) in contacts)Contact(bs[a],bs[b],true);}
  }
  for(int polish=0;polish<3000;polish++){foreach(var j in js)Solve(bs,j);foreach(var (a,b) in contacts)Contact(bs[a],bs[b],true);}
  float pivot=js.Max(j=>Vector3.Distance(bs[j.A].Point(j.PivotA),bs[j.B].Point(j.PivotB))),angle=js.Max(j=>AngularError(bs,j)),penetration=contacts.Max(p=>Contact(bs[p.A],bs[p.B],false));
  if(!float.IsFinite(Energy())||Energy()>=before||!float.IsFinite(pivot)||!float.IsFinite(angle)||!float.IsFinite(penetration)||pivot>.005f||angle>.002f||penetration>.001f)throw new InvalidDataException("Source cloth gravity constraints did not converge");
  var oldModel=FLVER2.Read(sourceModel);var bind=SkinRetarget.World(model.Nodes);var sourceNames=model.Nodes.Select((n,i)=>(n.Name,i)).ToDictionary(p=>p.Name,p=>p.i);var changes=new Dictionary<int,Matrix4x4>();
  var capeMeshes=Enumerable.Range(0,model.Meshes.Count).Where(i=>model.Materials[model.Meshes[i].MaterialIndex].Name=="#07#").ToArray();
  if(capeMeshes.Length!=2||capeMeshes.Sum(i=>model.Meshes[i].Vertices.Count)!=337)throw new InvalidDataException("Authored source cape mesh contract changed");
  var fixedNames=bs.Where(b=>b.InverseMass==0).Select(b=>b.Name).ToHashSet(StringComparer.Ordinal);var capeFixed=new HashSet<int>();
  foreach(int meshIndex in capeMeshes)foreach(var v in model.Meshes[meshIndex].Vertices)for(int slot=0;slot<4;slot++)if(v.BoneWeights[slot]>0) {
   int at=SkinRetarget.Bone(model.Meshes[meshIndex],v.BoneIndices[slot]),steps=0;
   while(at>=0&&!fixedNames.Contains(model.Nodes[at].Name)){if(++steps>model.Nodes.Count)throw new InvalidDataException("Cyclic source cape bind hierarchy");at=model.Nodes[at].ParentIndex;}
   if(at<0)throw new InvalidDataException("Source cape weight has no authored fixed capsule ancestor");capeFixed.Add(at);
  }
  List<int> Ancestors(int at) {var chain=new List<int>();while(at>=0){if(chain.Count>=model.Nodes.Count)throw new InvalidDataException("Cyclic source cape anchor hierarchy");chain.Add(at);at=model.Nodes[at].ParentIndex;}return chain;}
  var fixedChains=capeFixed.Select(Ancestors).ToArray();int capeAnchor=fixedChains[0].FirstOrDefault(i=>fixedChains.All(chain=>chain.Contains(i)),-1);
  if(capeAnchor<0||model.Nodes[capeAnchor].Name!="Spine1"||!capeFixed.Select(i=>model.Nodes[i].Name).ToHashSet(StringComparer.Ordinal).SetEquals(new[]{"L UpperArm","R UpperArm","Spine1"}))throw new InvalidDataException("Unsupported source cape fixed capsule anatomy");
  var anchors=capeMeshes.ToDictionary(i=>i,_=>model.Nodes[capeAnchor].Name);
  var used=model.Meshes.SelectMany(mesh=>mesh.Vertices.SelectMany(v=>Enumerable.Range(0,4).Where(i=>v.BoneWeights[i]>0).Select(i=>SkinRetarget.Bone(mesh,v.BoneIndices[i])))).ToHashSet();
  foreach(var b in bs.Where(b=>b.InverseMass>0))if(sourceNames.TryGetValue(b.Name,out int node)&&used.Contains(node)) {
   if(Vector3.Distance(bind[node].Translation,b.Initial.Translation)>.001f)throw new InvalidDataException("Sourcephysics/bodybone bind mismatch "+b.Name);
   if(!Matrix4x4.Invert(b.Initial,out var inverse))throw new InvalidDataException("Singular sourcecloth bind");changes.Add(node,inverse*b.World);
  }
  int changedVertices=0;
  for(int meshIndex=0;meshIndex<model.Meshes.Count;meshIndex++){var mesh=model.Meshes[meshIndex];var old=oldModel.Meshes[meshIndex];for(int vertexIndex=0;vertexIndex<mesh.Vertices.Count;vertexIndex++) {
   var v=mesh.Vertices[vertexIndex];var original=old.Vertices[vertexIndex];
   if(!Enumerable.Range(0,4).Any(i=>original.BoneWeights[i]>0&&changes.ContainsKey(SkinRetarget.Bone(mesh,original.BoneIndices[i]))))continue;
   var position=Vector3.Zero;var normal=Vector3.Zero;var tangents=new Vector3[v.Tangents.Count];float sum=0;
   for(int slot=0;slot<4;slot++){float weight=original.BoneWeights[slot];if(weight<=0)continue;int bone=SkinRetarget.Bone(mesh,original.BoneIndices[slot]);var delta=changes.GetValueOrDefault(bone,Matrix4x4.Identity);position+=Vector3.Transform(original.Position,delta)*weight;normal+=Vector3.TransformNormal(original.Normal,delta)*weight;for(int t=0;t<tangents.Length;t++)tangents[t]+=Vector3.TransformNormal(new Vector3(original.Tangents[t].X,original.Tangents[t].Y,original.Tangents[t].Z),delta)*weight;sum+=weight;}
   if(!float.IsFinite(sum)||Math.Abs(sum-1)>.001f||!float.IsFinite(position.LengthSquared())||normal.LengthSquared()<1e-12f)throw new InvalidDataException("Invalid source cloth skin weights/geometry");
   if(Vector3.Distance(position/sum,original.Position)>.00001f)changedVertices++;v.Position=position/sum;v.Normal=Vector3.Normalize(normal);for(int t=0;t<tangents.Length;t++){if(new Vector3(original.Tangents[t].X,original.Tangents[t].Y,original.Tangents[t].Z).LengthSquared()<1e-12f)continue;var tangent=tangents[t]-v.Normal*Vector3.Dot(v.Normal,tangents[t]);if(tangent.LengthSquared()<1e-12f||!float.IsFinite(tangent.LengthSquared()))throw new InvalidDataException("Invalid source cloth tangent");v.Tangents[t]=new Vector4(Vector3.Normalize(tangent),original.Tangents[t].W);}
  }}
  foreach(var mesh in model.Meshes)if(mesh.BoundingBox!=null){mesh.BoundingBox.Min=mesh.Vertices.Select(v=>v.Position).Aggregate(Vector3.Min);mesh.BoundingBox.Max=mesh.Vertices.Select(v=>v.Position).Aggregate(Vector3.Max);}
  model.Header.BoundingBoxMin=model.Meshes.SelectMany(m=>m.Vertices).Select(v=>v.Position).Aggregate(Vector3.Min);model.Header.BoundingBoxMax=model.Meshes.SelectMany(m=>m.Vertices).Select(v=>v.Position).Aggregate(Vector3.Max);
  var result=model.Write();var round=FLVER2.Read(result);if(round.Meshes.Sum(m=>m.Vertices.Count)!=oldModel.Meshes.Sum(m=>m.Vertices.Count))throw new InvalidDataException("Source cloth serialization lost vertices");
  return new(result,new(Convert.ToHexStringLower(SHA256.HashData(sourcePhysics)),bs.Count,bs.Count(b=>b.InverseMass>0),js.Count,contacts.Count,1500,3000,changedVertices,before,Energy(),pivot,angle,penetration,bs.Select(b=>new BodyPose(b.Name,b.InverseMass==0,b.Initial,b.World)).ToArray(),model.Nodes[capeAnchor].Name,capeFixed.Select(i=>model.Nodes[i].Name).Order(StringComparer.Ordinal).ToArray(),capeMeshes,"Offline capsule-cloth gravity pose using source masses, inertia, joint pivots, angular bounds and capsule contacts. The static cape follows the common torso ancestor of its authored fixed capsule anchors; independent arm motion cannot deform it. Havok dynamics, velocity/friction, wind, and ongoing cloth/body interactions are not reproduced."),anchors);
 }
}
