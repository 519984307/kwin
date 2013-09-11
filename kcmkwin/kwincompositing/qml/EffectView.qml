/**************************************************************************
* KWin - the KDE window manager                                          *
* This file is part of the KDE project.                                  *
*                                                                        *
* Copyright (C) 2013 Antonis Tsiapaliokas <kok3rs@gmail.com>             *
*                                                                        *
* This program is free software; you can redistribute it and/or modify   *
* it under the terms of the GNU General Public License as published by   *
* the Free Software Foundation; either version 2 of the License, or      *
* (at your option) any later version.                                    *
*                                                                        *
* This program is distributed in the hope that it will be useful,        *
* but WITHOUT ANY WARRANTY; without even the implied warranty of         *
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the          *
* GNU General Public License for more details.                           *
*                                                                        *
* You should have received a copy of the GNU General Public License      *
* along with this program.  If not, see <http://www.gnu.org/licenses/>.  *
**************************************************************************/

import QtQuick 2.1
import QtQuick.Controls 1.0
import QtQuick.Layouts 1.0
import org.kde.kwin.kwincompositing 1.0
import org.kde.plasma.components 2.0 as PlasmaComponents

Item {

    Component {
        id: sectionHeading
        Rectangle {
            width: parent.width
            height:25
            color: "white"

            Text {
                text: section
                font.bold: true
                font.pointSize: 16
                color: "gray"
            }
        }
    }

    CompositingType {
        id: compositingType
    }

    RowLayout {
        id: row
        width: parent.width
        height: parent.height
        CheckBox {
            id: windowManagement
            text: i18n("Improved Window Management")
            checked: false
            anchors.left: col.right
            anchors.top: parent.top
            anchors.topMargin: col.height/2
            onClicked: searchModel.enableWidnowManagement(windowManagement.checked)
        }

        ComboBox {
            id: openGLType
            model: compositingType
            currentIndex: compositingType.currentOpenGLType()
            anchors.top: windowManagement.bottom
            anchors.left: col.right
            onCurrentIndexChanged: apply.enabled = true
        }

        Label {
            id: animationText
            text: i18n("Animation Speed:")
            anchors {
                top: openGLType.bottom
                horizontalCenter: windowManagement.horizontalCenter
                topMargin: 20
            }
        }

        Slider {
            id: animationSpeed
            maximumValue: 6.0
            stepSize: 1.0
            tickmarksEnabled: true
            value: compositing.animationSpeed
            anchors {
                top: animationText.bottom
                left: col.right
            }

            onValueChanged: {
                apply.enabled = true;
            }
        }

        ColumnLayout {
            id: col
            height: parent.height
            Layout.minimumWidth: parent.width - windowManagement.width

            anchors {
                top: parent.top
                left: parent.left
            }

            Label {
                id: hint
                text: i18n("Hint: To find out or configure how to activate an effect, look at the effect's settings.")
                anchors {
                    top: parent.top
                    left: parent.left
                }
            }

            PlasmaComponents.TextField {
                id: searchField
                clearButtonShown: true
                Layout.fillWidth: true
                height: 20
                anchors {
                    top: hint.bottom
                }
                focus: true
            }

            EffectFilterModel {
                id: searchModel
                filter: searchField.text
                signal effectState(int rowIndex, bool enabled)

                onEffectState: {
                    searchModel.updateEffectStatus(rowIndex, enabled);
                }
            }

            ScrollView {
                id: scroll
                highlightOnFocus: true
                Layout.fillWidth: true
                Layout.fillHeight: true
                anchors {
                    top: searchField.bottom
                    left: parent.left
                    bottom: apply.top
                }
                ListView {
                    id: effectView
                    Layout.fillWidth: true
                    anchors.fill: parent
                    model: searchModel
                    delegate: Effect{}

                    section.property: "CategoryRole"
                    section.delegate: sectionHeading
                }
            }

            ExclusiveGroup {
                id: desktopSwitching
                //Our ExclusiveGroup must me outside of the
                //ListView, otherwise it will not work
            }

            Button {
                id: apply
                text: i18n("Apply")
                enabled: false
                anchors {
                    bottom: parent.bottom
                }

                onClicked: {
                    searchModel.syncConfig();
                    apply.enabled = false;
                    compositingType.syncConfig(openGLType.currentIndex, animationSpeed.value);
                }
            }

        }//End ColumnLayout
    }//End RowLayout
}//End item
