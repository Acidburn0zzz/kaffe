/* gnu.classpath.tools.gjdoc.PackageDocImpl
   Copyright (C) 2001 Free Software Foundation, Inc.

This file is part of GNU Classpath.

GNU Classpath is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2, or (at your option)
any later version.
 
GNU Classpath is distributed in the hope that it will be useful, but
WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
General Public License for more details.

You should have received a copy of the GNU General Public License
along with GNU Classpath; see the file COPYING.  If not, write to the
Free Software Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA
02111-1307 USA. */

package gnu.classpath.tools.gjdoc;

import com.sun.javadoc.*;
import java.util.*;

class PackageDocImpl extends DocImpl implements PackageDoc {

   private String packageName;
   private List   allClassesList      = new ArrayList();
   private List   ordinaryClassesList = new ArrayList();
   private List   exceptionsList      = new ArrayList();
   private List   interfacesList      = new ArrayList();
   private List   errorsList          = new ArrayList();
   
   PackageDocImpl(String packageName) {
      this.packageName=packageName;
   }

   public void addClass(ClassDoc classDoc) {
      if (Main.getInstance().includeAccessLevel(((ClassDocImpl)classDoc).accessLevel)) {
	 allClassesList.add(classDoc);
      }
   }

   public void resolve() {

      for (Iterator it=allClassesList.iterator(); it.hasNext(); ) {
	 ClassDocImpl classDoc=(ClassDocImpl)it.next();
	 try {
	     classDoc.resolve();
	 } catch (ParseException e) {
	     System.err.println("FIXME: add try-catch to force compilation"
				+ e);
	 }

	 if (classDoc.isInterface()) {
	    interfacesList.add(classDoc);
	 }
	 else if (classDoc.isException()) {
	    exceptionsList.add(classDoc);
	 }
	 else if (classDoc.isError()) {
	    errorsList.add(classDoc);
	 }
	 else {
	    ordinaryClassesList.add(classDoc);
	 }
      }
   }

   public void resolveComments() {
      if (rawDocumentation!=null) {
	 this.tagMap=parseCommentTags(rawDocumentation.toCharArray(),
				      0,
				      rawDocumentation.length(),
				      null);
      }

      resolveTags();
   }

   public String name() { 
      return packageName; 
   }

   public ClassDoc[] allClasses()      { return (ClassDoc[])allClassesList.toArray(new ClassDoc[0]); }
   public ClassDoc[] ordinaryClasses() { return (ClassDoc[])ordinaryClassesList.toArray(new ClassDoc[0]); }
   public ClassDoc[] exceptions()      { return (ClassDoc[])exceptionsList.toArray(new ClassDoc[0]); }
   public ClassDoc[] interfaces()      { return (ClassDoc[])interfacesList.toArray(new ClassDoc[0]); }
   public ClassDoc[] errors()          { return (ClassDoc[])errorsList.toArray(new ClassDoc[0]); }
   public ClassDoc   findClass(String name) { 
      return Main.getRootDoc().classNamed(packageName+"."+name);
   }

   public void dump(int level) {
      Debug.log(level, "All classes:");
      Debug.dumpArray(level, allClasses());

      Debug.log(level, "Ordinary classes:");
      Debug.dumpArray(level, ordinaryClasses());
      
   }

   public static final PackageDocImpl DEFAULT_PACKAGE = new PackageDocImpl("");

   public boolean isPackage() {
      return true;
   }

   public boolean isIncluded() {
      return isIncluded;
   }

   void setIsIncluded(boolean b) {
      this.isIncluded=b;
   }

   private boolean isIncluded = false;

   public String toString() {
      return packageName;
   }

   public int compareTo(Object o) {
      if (o!=null && o instanceof PackageDocImpl)
	 return name().compareTo(((PackageDocImpl)o).name());
      else
	 return 0;
   }
}
