/*
 * custompropertieshelper.cpp
 * Copyright 2021, Thorbjørn Lindeijer <bjorn@lindeijer.nl>
 *
 * This file is part of Tiled.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "custompropertieshelper.h"

#include "object.h"
#include "preferences.h"
#include "propertytype.h"
#include "variantpropertymanager.h"

namespace Tiled {

CustomPropertiesHelper::CustomPropertiesHelper(QtVariantPropertyManager *propertyManager,
                                               QObject *parent)
    : QObject(parent)
    , mPropertyManager(propertyManager)
{
    connect(propertyManager, &QtVariantPropertyManager::valueChanged,
            this, &CustomPropertiesHelper::valueChanged);

    connect(Preferences::instance(), &Preferences::propertyTypesChanged,
            this, &CustomPropertiesHelper::propertyTypesChanged);
}

QtVariantProperty *CustomPropertiesHelper::createProperty(const QString &name,
                                                          const QVariant &value)
{
    Q_ASSERT(!mProperties.contains(name));

    QtVariantProperty *property = createPropertyInternal(name, value);
    mProperties.insert(name, property);

    return property;
}

QtVariantProperty *CustomPropertiesHelper::createPropertyInternal(const QString &name, const QVariant &value)
{
    int type = value.userType();

    const PropertyType *propertyType = nullptr;

    if (type == propertyValueId()) {
        const PropertyValue propertyValue = value.value<PropertyValue>();
        propertyType = propertyValue.type();

        if (propertyType) {
            switch (propertyType->type) {
            case PropertyType::PT_Invalid:
                break;
            case PropertyType::PT_Class:
                type = VariantPropertyManager::unstyledGroupTypeId();
                break;
            case PropertyType::PT_Enum: {
                const auto &enumType = static_cast<const EnumPropertyType&>(*propertyType);
                if (enumType.valuesAsFlags)
                    type = QtVariantPropertyManager::flagTypeId();
                else
                    type = QtVariantPropertyManager::enumTypeId();
                break;
            }
            }
        }
    }

    if (type == objectRefTypeId())
        type = VariantPropertyManager::displayObjectRefTypeId();

    QtVariantProperty *property = mPropertyManager->addProperty(type, name);
    if (!property) {
        // fall back to string property for unsupported property types
        property = mPropertyManager->addProperty(QMetaType::QString, name);
    }

    if (type == QMetaType::Bool)
        property->setAttribute(QLatin1String("textVisible"), false);
    if (type == QMetaType::QString)
        property->setAttribute(QLatin1String("multiline"), true);
    if (type == QMetaType::Double)
        property->setAttribute(QLatin1String("decimals"), 9);

    if (propertyType) {
        mPropertyTypeIds.insert(property, propertyType->id);
        setPropertyAttributes(property, *propertyType);
    } else {
        mPropertyTypeIds.insert(property, 0);
    }

    // todo: this doesn't set value of child properties...
    property->setValue(toDisplayValue(value));

    return property;
}

void CustomPropertiesHelper::deleteProperty(QtProperty *property)
{
    Q_ASSERT(mPropertyTypeIds.contains(property));

    // not the case for child properties
    if (mProperties.value(property->propertyName()) == property)
        mProperties.remove(property->propertyName());

    mPropertyTypeIds.remove(property);
    mPropertyParents.remove(property);
    delete property;
}

void CustomPropertiesHelper::clear()
{
    qDeleteAll(mProperties);
    mProperties.clear();
    mPropertyTypeIds.clear();
}

QVariant CustomPropertiesHelper::toDisplayValue(QVariant value) const
{
    if (value.userType() == propertyValueId())
        value = value.value<PropertyValue>().value;

    if (value.userType() == objectRefTypeId())
        value = QVariant::fromValue(DisplayObjectRef { value.value<ObjectRef>(), mMapDocument });

    return value;
}

QVariant CustomPropertiesHelper::fromDisplayValue(QtProperty *property,
                                                  QVariant value) const
{
    if (value.userType() == VariantPropertyManager::displayObjectRefTypeId())
        value = QVariant::fromValue(value.value<DisplayObjectRef>().ref);

    if (const auto typeId = mPropertyTypeIds.value(property))
        if (auto type = Object::propertyTypes().findTypeById(typeId))
            value = type->wrap(value);

    return value;
}

void CustomPropertiesHelper::valueChanged(QtProperty *property, const QVariant &value)
{
    // This function handles changes in sub-properties only
    auto parent = static_cast<QtVariantProperty*>(mPropertyParents.value(property));
    if (!parent)
        return;

    auto variantMap = parent->value().toMap();
    qDebug() << "before:" << variantMap;
    variantMap.insert(property->propertyName(), fromDisplayValue(property, value));
    qDebug() << "after:" << variantMap;

    // This might trigger another call of this function, in case of recursive
    // class members.
    parent->setValue(variantMap);
}

void CustomPropertiesHelper::propertyTypesChanged()
{
    for (const auto &type : Object::propertyTypes()) {
        QHashIterator<QtProperty *, int> it(mPropertyTypeIds);
        while (it.hasNext()) {
            it.next();

            if (it.value() == type->id)
                setPropertyAttributes(it.key(), *type);
        }
    }
}

void CustomPropertiesHelper::setPropertyAttributes(QtProperty *property, const PropertyType &propertyType)
{
    switch (propertyType.type) {
    case Tiled::PropertyType::PT_Invalid:
    case Tiled::PropertyType::PT_Class: {
        const auto &classType = static_cast<const ClassPropertyType&>(propertyType);

        // Delete any existing sub-properties
        qDeleteAll(property->subProperties());

        // Set up new properties
        QMapIterator<QString, QVariant> it(classType.members);
        while (it.hasNext()) {
            it.next();
            const QString &name = it.key();
            const QVariant &value = it.value();

            QtVariantProperty *subProperty = createPropertyInternal(name, value);
            property->addSubProperty(subProperty);
            mPropertyParents.insert(subProperty, property);
        }

        // TODO: How to set the actual value on the sub-properties?
        break;
    }
    case Tiled::PropertyType::PT_Enum: {
        const auto &enumType = static_cast<const EnumPropertyType&>(propertyType);
        if (enumType.valuesAsFlags) {
            mPropertyManager->setAttribute(property, QStringLiteral("flagNames"), enumType.values);
        } else {
            // TODO: Support icons for enum values
            mPropertyManager->setAttribute(property, QStringLiteral("enumNames"), enumType.values);
        }
        break;
    }
    }
}

} // namespace Tiled

#include "moc_custompropertieshelper.cpp"
