#pragma once

struct AGlyph {
	QString name;
	QPainterPath path;    

    /* overload the operators */
    friend QDataStream& operator<< (QDataStream& out, const AGlyph& rhs)
    {
        out << rhs.name << rhs.path;
        return out;
    }

    friend QDataStream& operator>> (QDataStream& in, AGlyph& rhs)
    {
        in >> rhs.name >> rhs.path;
        return in;
    }
};