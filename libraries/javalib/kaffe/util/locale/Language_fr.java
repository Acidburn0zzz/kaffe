/*
 * Java core library component.
 *
 * Copyright (c) 2002
 *      Dalibor Topic <robilad@yahoo.com>.  All rights reserved.
 *
 * See the file "license.terms" for information on usage and redistribution
 * of this file.
 */

package kaffe.util.locale;

import java.util.ListResourceBundle;

public class Language_fr extends ListResourceBundle {
    public Object [] [] getContents() {
	return contents;
    }

    private Object [] [] contents = {
	{ "en_CA", "anglais" },
	{ "fr_CA", "fran�ais" },
	{ "zh_CN", "chinois" },
	{ "zh", "chinois" },
	{ "en", "anglais" },
	{ "fr_FR", "fran�ais" },
	{ "fr", "fran�ais" },
	{ "de", "allemand" },
	{ "de_DE", "allemand" },
	{ "it", "italien" },
	{ "it_IT", "italien" },
	{ "ja_JP", "japonais" },
	{ "ja", "japonais" },
	{ "ko_KR", "cor�en" },
	{ "ko", "cor�en" },
	{ "zh_TW", "chinois" },
	{ "en_GB", "anglais" },
	{ "en_US", "anglais" }
    };
}
