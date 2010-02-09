/*
 * Copyright 1999 by Martin R. Jones <mjones@kde.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */
#include "amor.h"
#include "amor.moc"
#include "amorpm.h"
#include "amorbubble.h"
#include "amorwidget.h"
#include "amordialog.h"
#include "version.h"
#include "amoradaptor.h"

#include <stdlib.h>
#include <unistd.h>
#include <time.h>

#include <QtDBus/QtDBus>
#include <QtCore/QTimer>
#include <QtGui/QCursor>

#include <kmenu.h>
#include <kapplication.h>
#include <kdebug.h>
#include <klocale.h>
#include <kmessagebox.h>
#include <kstartupinfo.h>
#include <kwindowsystem.h>
#include <kstandarddirs.h>
#include <khelpmenu.h>
#include <kiconloader.h>
#include <krandom.h>

#if defined Q_WS_X11
#include <X11/Xlib.h>
#include <QtGui/QX11Info>
#endif
// #define DEBUG_AMOR

#define SLEEP_TIMEOUT   180     // Animation sleeps after SLEEP_TIMEOUT seconds
                                // of mouse inactivity.
#define TIPS_FILE       "tips"  // Display tips in TIP_FILE-LANG, e.g "tips-en"
#define TIP_FREQUENCY   20      // Frequency tips are displayed small == more
                                // often.

#define BUBBLE_TIME_STEP 250

// Standard animation groups
#define ANIM_BASE       "Base"
#define ANIM_NORMAL     "Sequences"
#define ANIM_FOCUS      "Focus"
#define ANIM_BLUR       "Blur"
#define ANIM_DESTROY    "Destroy"
#define ANIM_SLEEP      "Sleep"
#define ANIM_WAKE       "Wake"

//---------------------------------------------------------------------------
// QueueItem
// Constructor
//

QueueItem::QueueItem(itemType ty, const QString &te, int ti)
{
    // if the time field was not given, calculate one based on the type
    // and length of the item
    int effectiveLength = 0, nesting = 0;

    // discard html code from the length count
    for (int i = 0; i < te.length(); i++)
    {
	if (te[i] == '<')	nesting++;
	else if (te[i] == '>')	nesting--;
	else if (!nesting)	effectiveLength++;
    }
    if (nesting) // malformed html
    {
#ifdef DEBUG_AMOR
	kDebug(10000) << "QueueItem::QueueItem(): Malformed HTML!";
#endif
	effectiveLength = te.length();
    }

    if (ti == -1)
    {
	switch (ty)  {
	    case Talk : // shorter times
			ti = 1500 + 45 * effectiveLength;
			break;
	    case Tip  : // longer times
			ti = 4000 + 30 * effectiveLength;
			break;
	}
    }

    iType = ty;
    iText = te;
    iTime = ti;
}

//---------------------------------------------------------------------------
// AMOR
// Constructor
//
Amor::Amor() : QObject()
{
    new AmorAdaptor(this);
    QDBusConnection::sessionBus().registerObject("/Amor", this);
    mAmor = 0;
    mBubble = 0;
    mForceHideAmorWidget = false;
    if (readConfig())
    {
        mTargetWin   = 0;
        mNextTarget  = 0;
        mAmorDialog  = 0;
        mMenu        = 0;
        mCurrAnim    = mBaseAnim;
        mPosition    = mCurrAnim->hotspot().x();
        mState       = Normal;

        mWin = KWindowSystem::self();
        connect(mWin, SIGNAL(activeWindowChanged(WId)),
                this, SLOT(slotWindowActivate(WId)));
        connect(mWin, SIGNAL(windowRemoved(WId)),
                this, SLOT(slotWindowRemove(WId)));
        connect(mWin, SIGNAL(stackingOrderChanged()),
                this, SLOT(slotStackingChanged()));
        connect(mWin, SIGNAL(windowChanged(WId, const unsigned long *)),
                this, SLOT(slotWindowChange(WId, const unsigned long *)));
        connect(mWin, SIGNAL(currentDesktopChanged(int)),
                this, SLOT(slotDesktopChange(int)));

        mAmor = new AmorWidget();
        connect(mAmor, SIGNAL(mouseClicked(const QPoint &)),
                        SLOT(slotMouseClicked(const QPoint &)));
        connect(mAmor, SIGNAL(dragged(const QPoint &, bool)),
                        SLOT(slotWidgetDragged(const QPoint &, bool)));
        mAmor->resize(mTheme.maximumSize());

        mTimer = new QTimer(this);
        connect(mTimer, SIGNAL(timeout()), SLOT(slotTimeout()));

        mStackTimer = new QTimer(this);
        connect(mStackTimer, SIGNAL(timeout()), SLOT(restack()));

	mBubbleTimer = new QTimer(this);
	connect(mBubbleTimer, SIGNAL(timeout()), SLOT(slotBubbleTimeout()));

        time(&mActiveTime);
        mCursPos = QCursor::pos();
        mCursorTimer = new QTimer(this);
        connect(mCursorTimer, SIGNAL(timeout()), SLOT(slotCursorTimeout()));
	mCursorTimer->start( 500 );

        if (mWin->activeWindow())
        {
            mNextTarget = mWin->activeWindow();
            selectAnimation(Focus);
            mTimer->setSingleShot(true);
            mTimer->start(0);
        }
        if ( !QDBusConnection::sessionBus().connect(QString(), QString(), "org.kde.amor",
                    "KDE_stop_screensaver", this, SLOT( screenSaverStopped()) ) )
		kDebug(10000) << "Could not attach signal...KDE_stop_screensaver()";
	else
		kDebug(10000) << "attached dbus signals...";

         if ( !QDBusConnection::sessionBus().connect(QString(), QString(), "org.kde.amor", "KDE_start_screensaver", this, SLOT( screenSaverStarted()) ) )
		kDebug(10000) << "Could not attach signal...KDE_start_screensaver()";
	else
		kDebug(10000) << "attached dbus signals...";


	KStartupInfo::appStarted();
    }
    else
    {
        qApp->quit();
    }
}

//---------------------------------------------------------------------------
//
// Destructor
//
Amor::~Amor()
{
	qDeleteAll(mTipsQueue);
	mTipsQueue.clear();
    delete mAmor;
    delete mBubble;
}

void Amor::screenSaverStopped()
{
#ifdef DEBUG_AMOR
    kDebug(10000)<<"void Amor::screenSaverStopped() \n";
#endif

    mAmor->show();
    mForceHideAmorWidget = false;

    mTimer->setSingleShot(true);
    mTimer->start(0);
}

void Amor::screenSaverStarted()
{
#ifdef DEBUG_AMOR
    kDebug(10000)<<"void Amor::screenSaverStarted() \n";
#endif

    mAmor->hide();
    mTimer->stop();
    mForceHideAmorWidget = true;

    // GP: hide the bubble (if there's any) leaving any current message in the queue
    hideBubble();
}

//---------------------------------------------------------------------------
//
void Amor::showTip( const QString &tip )
{
    if (mTipsQueue.count() < 5 && !mForceHideAmorWidget) // start dropping tips if the queue is too long
        mTipsQueue.enqueue(new QueueItem(QueueItem::Tip, tip));

    if (mState == Sleeping)
    {
	selectAnimation(Waking);	// Set waking immediatedly
    mTimer->setSingleShot(true);
	mTimer->start(0);
    }
}


void Amor::showMessage( const QString &message )
{
    showMessage(message, -1);
}

void Amor::showMessage( const QString &message , int msec )
{
    // FIXME: What should be done about messages and tips while the screensaver is on?
    if (mForceHideAmorWidget) return; // do not show messages sent while in the screensaver

    mTipsQueue.enqueue(new QueueItem(QueueItem::Talk, message, msec));

    if (mState == Sleeping)
    {
	selectAnimation(Waking);	// Set waking immediatedly
    mTimer->setSingleShot(true);
	mTimer->start(0);
    }
}


//---------------------------------------------------------------------------
//
// Clear existing theme and reload configuration
//
void Amor::reset()
{
    hideBubble();

    mAmor->setPixmap(0L);	// get rid of your old copy of the pixmap

    AmorPixmapManager::manager()->reset();
    mTips.reset();

//    mTipsQueue.clear();	Why had I chosen to clean the tips queue? insane!

    readConfig();

    mCurrAnim   = mBaseAnim;
    mPosition   = mCurrAnim->hotspot().x();
    mState      = Normal;

    mAmor->resize(mTheme.maximumSize());
    mCurrAnim->reset();

    mTimer->setSingleShot(true);
    mTimer->start(0);
}

//---------------------------------------------------------------------------
//
// Read the selected theme.
//
bool Amor::readConfig()
{
    // Read user preferences
    mConfig.read();

    if (mConfig.mTips)
    {
        mTips.setFile(TIPS_FILE);
    }

    // Select a random theme if user requested it
    if (mConfig.mRandomTheme)
    {
        QStringList files;

        // Store relative paths into files to avoid storing absolute pathnames.
        KGlobal::dirs()->findAllResources("appdata", "*rc", KStandardDirs::NoSearchOptions, files);
        int randomTheme = KRandom::random() % files.count();
        mConfig.mTheme = files.at(randomTheme);
    }

    // read selected theme
    if (!mTheme.setTheme(mConfig.mTheme))
    {
        KMessageBox::error(0, i18nc("@info:status", "Error reading theme: ") + mConfig.mTheme);
        return false;
    }

    if ( !mTheme.isStatic() )
    {
	const char *groups[] = { ANIM_BASE, ANIM_NORMAL, ANIM_FOCUS, ANIM_BLUR,
				ANIM_DESTROY, ANIM_SLEEP, ANIM_WAKE, 0 };

	// Read all the standard animation groups
	for (int i = 0; groups[i]; i++)
	{
	    if (mTheme.readGroup(groups[i]) == false)
	    {
		KMessageBox::error(0, i18nc("@info:status", "Error reading group: ") + groups[i]);
		return false;
	    }
	}
    }
    else
    {
	if ( mTheme.readGroup( ANIM_BASE ) == false )
	{
	    KMessageBox::error(0, i18nc("@info:status", "Error reading group: ") + ANIM_BASE);
	    return false;
	}
    }

    // Get the base animation
    mBaseAnim = mTheme.random(ANIM_BASE);

    return true;
}

//---------------------------------------------------------------------------
//
// Show the bubble text
//
void Amor::showBubble()
{
    if (!mTipsQueue.isEmpty())
    {
#ifdef DEBUG_AMOR
    kDebug(10000) << "Amor::showBubble(): Displaying tips bubble.";
#endif

        if (!mBubble)
        {
            mBubble = new AmorBubble;
        }

        mBubble->setOrigin(mAmor->x()+mAmor->width()/2,
                           mAmor->y()+mAmor->height()/2);
        mBubble->setMessage(mTipsQueue.head()->text());

//	mBubbleTimer->start(mTipsQueue.head()->time(), true);
    mBubbleTimer->setSingleShot(true);
	mBubbleTimer->start(BUBBLE_TIME_STEP);
    }
}

//---------------------------------------------------------------------------
//
// Hide the bubble text if visible
//
void Amor::hideBubble(bool forceDequeue)
{
    if (mBubble)
    {
#ifdef DEBUG_AMOR
    kDebug(10000) << "Amor::hideBubble(): Hiding tips bubble";
#endif

        // GP: stop mBubbleTimer to avoid deleting the first element, just in case we are changing windows
	// or something before the tip was shown long enough
        mBubbleTimer->stop();

	// GP: the first message on the queue should be taken off for a
	// number of reasons: a) forceDequeue == true, only when called
	// from slotBubbleTimeout; b) the bubble is not visible ; c)
	// the bubble is visible, but there's Tip being displayed. The
	// latter is to keep backwards compatibility and because
	// carrying around a tip bubble when switching windows quickly is really
	// annoyying
	if (forceDequeue || !mBubble->isVisible() ||
	    (mTipsQueue.head()->type() == QueueItem::Tip)) /* there's always an item in the queue here */
	    mTipsQueue.dequeue();

        delete mBubble;
        mBubble = 0;
    }
}

//---------------------------------------------------------------------------
//
// Select a new animation appropriate for the current state.
//
void Amor::selectAnimation(State state)
{
    switch (state)
    {
        case Blur:
            hideBubble();
            mCurrAnim = mTheme.random(ANIM_BLUR);
            mState = Focus;
            break;

        case Focus:
            hideBubble();
            mCurrAnim = mTheme.random(ANIM_FOCUS);
            mCurrAnim->reset();
            mTargetWin = mNextTarget;
            if (mTargetWin != None)
            {
                mTargetRect = KWindowSystem::windowInfo(mTargetWin, NET::WMFrameExtents).frameGeometry();

		// if the animation falls outside of the working area,
		// then relocate it so that is inside the desktop again
		QRect desktopArea = mWin->workArea();
		mInDesktopBottom = false;

		if (mTargetRect.y() - mCurrAnim->hotspot().y() + mConfig.mOffset <
		    desktopArea.y())
		{
		    // relocate the animation at the bottom of the screen
		    mTargetRect = QRect(desktopArea.x(),
				  desktopArea.y() + desktopArea.height(),
				  desktopArea.width(), 0);

		    // we'll relocate the animation in the desktop
		    // frame, so do not add the offset to its vertical position
		    mInDesktopBottom = true;
		}

		if ( mTheme.isStatic() )
		{
		    if ( mConfig.mStaticPos < 0 )
			mPosition = mTargetRect.width() + mConfig.mStaticPos;
		    else
			mPosition = mConfig.mStaticPos;
		    if ( mPosition >= mTargetRect.width() )
			mPosition = mTargetRect.width()-1;
		    else if ( mPosition < 0 )
			mPosition = 0;
		}
		else
		{
		    if (mCurrAnim->frame())
		    {
		        if (mTargetRect.width() == mCurrAnim->frame()->width())
			    mPosition = mCurrAnim->hotspot().x();
			else
			    mPosition = ( KRandom::random() %
					  (mTargetRect.width() - mCurrAnim->frame()->width()) )
					 + mCurrAnim->hotspot().x();
		    }
		    else
		    {
			mPosition = mTargetRect.width()/2;
		    }
		}
            }
            else
            {
                // We don't want to do anything until a window comes into
                // focus.
                mTimer->stop();
            }
            mAmor->hide();

            restack();
            mState = Normal;
            break;

        case Destroy:
            hideBubble();
            mCurrAnim = mTheme.random(ANIM_DESTROY);
            mState = Focus;
            break;

        case Sleeping:
            mCurrAnim = mTheme.random(ANIM_SLEEP);
            break;

        case Waking:
            mCurrAnim = mTheme.random(ANIM_WAKE);
            mState = Normal;
            break;

        default:
            // Select a random normal animation if the current animation
            // is not the base, otherwise select the base.  This makes us
            // alternate between the base animation and a random
            // animination.
	    if (mCurrAnim == mBaseAnim && !mBubble)
            {
                mCurrAnim = mTheme.random(ANIM_NORMAL);
            }
            else
            {
                mCurrAnim = mBaseAnim;
            }
            break;
    }

    if (mCurrAnim->totalMovement() + mPosition > mTargetRect.width() ||
        mCurrAnim->totalMovement() + mPosition < 0)
    {
        // The selected animation would end outside of this window's width
        // We could randomly select a different one, but I prefer to just
        // use the default animation.
        mCurrAnim = mBaseAnim;
    }

    mCurrAnim->reset();
}

//---------------------------------------------------------------------------
//
// Set the animation's stacking order to be just above the target window's
// window decoration, or on top.
//
void Amor::restack()
{
    if (mTargetWin == None)
    {
        return;
    }

    if (mConfig.mOnTop)
    {
        // simply raise the widget to the top
        mAmor->raise();
        return;
    }

#ifdef DEBUG_AMOR
    kDebug(10000) << "restacking";
#endif
#if defined Q_WS_X11
    Window sibling = mTargetWin;
    Window dw, parent = None, *wins;

    do {
        unsigned int nwins = 0;

        // We must use the target window's parent as our sibling.
        // Is there a faster way to get parent window than XQueryTree?
        if (XQueryTree(QX11Info::display(), sibling, &dw, &parent, &wins, &nwins))
        {
            if (nwins)
            {
                XFree(wins);
            }
        }

        if (parent != None && parent != dw )
            sibling = parent;
    } while ( parent != None && parent != dw );

    // Set animation's stacking order to be above the window manager's
    // decoration of target window.
    XWindowChanges values;
    values.sibling = sibling;
    values.stack_mode = Above;
    XConfigureWindow(QX11Info::display(), mAmor->winId(), CWSibling | CWStackMode,
                     &values);
#endif
}

//---------------------------------------------------------------------------
//
// The user clicked on our animation.
//
void Amor::slotMouseClicked(const QPoint &pos)
{
    bool restartTimer = mTimer->isActive();

    // Stop the animation while the menu is open.
    if (restartTimer)
    {
        mTimer->stop();
    }

    if (!mMenu)
    {
        KHelpMenu* help = new KHelpMenu(0, KGlobal::mainComponent().aboutData(), false);
        KMenu* helpMenu = help->menu();
#ifdef __GNUC__
#warning the following is kinda dirty and should be done by KHelpMenu::menu() I think. (hermier)
#endif
        helpMenu->setIcon(SmallIcon("help-contents"));
        helpMenu->setTitle(i18nc("@action:inmenu Amor", "&Help"));

        mMenu = new KMenu(0);
        mMenu->addTitle("Amor"); // I really don't want this i18n'ed
        mMenu->addAction(SmallIcon("configure"), i18nc("@action:inmenu Amor", "&Configure..."), this, SLOT(slotConfigure()));
        mMenu->addSeparator();
        mMenu->addMenu(helpMenu);
        mMenu->addAction(SmallIcon("application-exit"), i18nc("@action:inmenu Amor", "&Quit"), kapp, SLOT(quit()));
    }

    mMenu->exec(pos);

    if (restartTimer)
    {

        mTimer->setSingleShot(true);
        mTimer->start(1000);
    }
}

//---------------------------------------------------------------------------
//
// Check cursor position
//
void Amor::slotCursorTimeout()
{
    QPoint currPos = QCursor::pos();
    QPoint diff = currPos - mCursPos;
    time_t now = time(0);

    if (mForceHideAmorWidget) return; // we're hidden, do nothing

    if (abs(diff.x()) > 1 || abs(diff.y()) > 1)
    {
	if (mState == Sleeping)
	{
	    // Set waking immediatedly
	    selectAnimation(Waking);
	}
	mActiveTime = now;
	mCursPos = currPos;
    }
    else if (mState != Sleeping && now - mActiveTime > SLEEP_TIMEOUT)
    {
	// GP: can't go to sleep if there are tips in the queue
	if (mTipsQueue.isEmpty())
	    mState = Sleeping;	// The next animation will become sleeping
    }
}

//---------------------------------------------------------------------------
//
// Display the next frame or a new animation
//
void Amor::slotTimeout()
{
    if ( mForceHideAmorWidget )
        return;

    if (!mTheme.isStatic())
	mPosition += mCurrAnim->movement();
    mAmor->setPixmap(mCurrAnim->frame());
    mAmor->move(mTargetRect.x() + mPosition - mCurrAnim->hotspot().x(),
                 mTargetRect.y() - mCurrAnim->hotspot().y() + (!mInDesktopBottom?mConfig.mOffset:0));
    if (!mAmor->isVisible())
    {
        mAmor->show();
        restack();
    }

    if (mCurrAnim == mBaseAnim && mCurrAnim->validFrame())
    {
	// GP: Application tips/messages can be shown in any frame number; amor tips are
	// only displayed on the first frame of mBaseAnim (the old way of doing this).
	if ( !mTipsQueue.isEmpty() && !mBubble &&  mConfig.mAppTips)
	    showBubble();
	else if (KRandom::random()%TIP_FREQUENCY == 1 && mConfig.mTips && !mBubble && !mCurrAnim->frameNum())
        {
	    mTipsQueue.enqueue(new QueueItem(QueueItem::Tip, mTips.tip()));
	    showBubble();
        }
    }

    if (mTheme.isStatic()) {
	    mTimer->setSingleShot(true);
        mTimer->start((mState == Normal) || (mState == Sleeping) ? 1000 : 100);
    }
    else {
        mTimer->setSingleShot(true);
	    mTimer->start(mCurrAnim->delay());
    }
    if (!mCurrAnim->next())
    {
	if ( mBubble )
	    mCurrAnim->reset();
	else
	    selectAnimation(mState);
    }
}

//---------------------------------------------------------------------------
//
// Display configuration dialog
//
void Amor::slotConfigure()
{
    if (!mAmorDialog)
    {
        mAmorDialog = new AmorDialog();
        connect(mAmorDialog, SIGNAL(changed()), SLOT(slotConfigChanged()));
        connect(mAmorDialog, SIGNAL(offsetChanged(int)),
                SLOT(slotOffsetChanged(int)));
    }

    mAmorDialog->show();
}

//--------------------------------------------------------------------------
//
// Configuration changed.
//
void Amor::slotConfigChanged()
{
    reset();
}

//---------------------------------------------------------------------------
//
// Offset changed
//
void Amor::slotOffsetChanged(int off)
{
    mConfig.mOffset = off;

    if (mCurrAnim->frame())
    {
        mAmor->move(mPosition + mTargetRect.x() - mCurrAnim->hotspot().x(),
                 mTargetRect.y() - mCurrAnim->hotspot().y() + (!mInDesktopBottom?mConfig.mOffset:0));
    }
}

//---------------------------------------------------------------------------
//
// Display About box
//
void Amor::slotAbout()
{
    QString about = i18nc("@label:textbox", "Amor Version %1\n\n", QString::fromLatin1(AMOR_VERSION)) +
                i18nc("@label:textbox", "Amusing Misuse Of Resources\n\n") +
                i18nc("@label:textbox", "Copyright 1999 Martin R. Jones <email>mjones@kde.org</email>\n\n") +
		i18nc("@label:textbox", "Original Author: Martin R. Jones <email>mjones@kde.org</email>\n") +
		i18nc("@label:textbox", "Current Maintainer: Gerardo Puga <email>gpuga@gioia.ing.unlp.edu.ar</email>\n" ) +
                "\nhttp://www.powerup.com.au/~mjones/amor/";
    KMessageBox::about(0, about, i18nc("@label:textbox", "About Amor"));
}

//---------------------------------------------------------------------------
//
// Widget dragged
//
void Amor::slotWidgetDragged( const QPoint &delta, bool release )
{
    if (mCurrAnim->frame())
    {
	int newPosition = mPosition + delta.x();
	if (mCurrAnim->totalMovement() + newPosition > mTargetRect.width())
	    newPosition = mTargetRect.width() - mCurrAnim->totalMovement();
	else if (mCurrAnim->totalMovement() + newPosition < 0)
	    newPosition = -mCurrAnim->totalMovement();
	mPosition = newPosition;
        mAmor->move(mTargetRect.x() + mPosition - mCurrAnim->hotspot().x(),
                 mAmor->y());

	if ( mTheme.isStatic() && release ) {
	    // static animations save the new position as preferred.
	    int savePos = mPosition;
	    if ( savePos > mTargetRect.width()/2 )
		savePos -= (mTargetRect.width()+1);

	    mConfig.mStaticPos = savePos;
	    mConfig.write();
	}
    }
}

//---------------------------------------------------------------------------
//
// Focus changed to a different window
//
void Amor::slotWindowActivate(WId win)
{
#ifdef DEBUG_AMOR
    kDebug(10000) << "Window activated:" << win;
#endif

    mTimer->stop();
    mNextTarget = win;

    // This is an active event that affects the target window
    time(&mActiveTime);

    // A window gaining focus implies that the current window has lost
    // focus.  Initiate a blur event if there is a current active window.
    if (mTargetWin)
    {
        // We are losing focus from the current window
        selectAnimation(Blur);
        mTimer->setSingleShot(true);
        mTimer->start(0);
    }
    else if (mNextTarget)
    {
        // We are setting focus to a new window
        if (mState != Focus )
	    selectAnimation(Focus);
        mTimer->setSingleShot(true);
    	mTimer->start(0);
    }
    else
    {
        // No action - We can get this when we switch between two empty
        // desktops
        mAmor->hide();
    }
}

//---------------------------------------------------------------------------
//
// Window removed
//
void Amor::slotWindowRemove(WId win)
{
#ifdef DEBUG_AMOR
    kDebug(10000) << "Window removed";
#endif

    if (win == mTargetWin)
    {
        // This is an active event that affects the target window
        time(&mActiveTime);

        selectAnimation(Destroy);
        mTimer->stop();
        mTimer->setSingleShot(true);
        mTimer->start(0);
    }
}

//---------------------------------------------------------------------------
//
// Window stacking changed
//
void Amor::slotStackingChanged()
{
#ifdef DEBUG_AMOR
    kDebug(10000) << "Stacking changed";
#endif

    // This is an active event that affects the target window
    time(&mActiveTime);

    // We seem to get this signal before the window has been restacked,
    // so we just schedule a restack.
    mStackTimer->setSingleShot(true);
    mStackTimer->start( 20);
}

//---------------------------------------------------------------------------
//
// Properties of a window changed
//
void Amor::slotWindowChange(WId win, const unsigned long * properties)
{

    if (win != mTargetWin)
    {
        return;
    }

    // This is an active event that affects the target window
    time(&mActiveTime);

    NET::MappingState mappingState = KWindowSystem::windowInfo( mTargetWin, NET::WMFrameExtents ).mappingState();

    if (mappingState == NET::Iconic ||
        mappingState == NET::Withdrawn)
    {
#ifdef DEBUG_AMOR
        kDebug(10000) << "Target window iconified";
#endif

        // The target window has been iconified
        selectAnimation(Destroy);
        mTargetWin = None;
        mTimer->stop();
        mTimer->setSingleShot(true);
        mTimer->start(0);

	return;
    }

    if (properties[0] & NET::WMGeometry)
    {
#ifdef DEBUG_AMOR
        kDebug(10000) << "Target window moved or resized";
#endif

        QRect newTargetRect = KWindowSystem::windowInfo(mTargetWin, NET::WMFrameExtents).frameGeometry();

	// if the change in the window caused the animation to fall
	// out of the working area of the desktop, or if the animation
	// didn't fall in the working area before but it does now, then
	//  refocus on the current window so that the animation is
	// relocated.
	QRect desktopArea = mWin->workArea();

	bool fitsInWorkArea = !(newTargetRect.y() - mCurrAnim->hotspot().y() + mConfig.mOffset < desktopArea.y());
	if ((!fitsInWorkArea && !mInDesktopBottom) || (fitsInWorkArea && mInDesktopBottom))
	{
	    mNextTarget = mTargetWin;
	    selectAnimation(Blur);
        mTimer->setSingleShot(true);
	    mTimer->start(0);

	    return;
	}

	if (!mInDesktopBottom)
	    mTargetRect = newTargetRect;

        // make sure the animation is still on the window.
        if (mCurrAnim->frame())
        {
            hideBubble();
	    if (mTheme.isStatic())
	    {
		if ( mConfig.mStaticPos < 0 )
		    mPosition = mTargetRect.width() + mConfig.mStaticPos;
		else
		    mPosition = mConfig.mStaticPos;
		if ( mPosition >= mTargetRect.width() )
		    mPosition = mTargetRect.width()-1;
		else if ( mPosition < 0 )
		    mPosition = 0;
	    }
            else if (mPosition > mTargetRect.width() -
                    (mCurrAnim->frame()->width() - mCurrAnim->hotspot().x()))
            {
                mPosition = mTargetRect.width() - (mCurrAnim->frame()->width() - mCurrAnim->hotspot().x());
            }
            mAmor->move(mTargetRect.x() + mPosition - mCurrAnim->hotspot().x(),
                     mTargetRect.y() - mCurrAnim->hotspot().y() + (!mInDesktopBottom?mConfig.mOffset:0));
        }

	return;
    }
}

//---------------------------------------------------------------------------
//
// Changed to a different desktop
//
void Amor::slotDesktopChange(int desktop)
{
    // GP: signal currentDesktopChanged seems to be emitted even if you
    // change to the very same desktop you are in.
    if (mWin->currentDesktop() == desktop)
	return;

#ifdef DEBUG_AMOR
    kDebug(10000) << "Desktop change";
#endif

    mNextTarget = None;
    mTargetWin = None;
    selectAnimation( Normal );
    mTimer->stop();
    mAmor->hide();
}

// GP ===========================================================================

void Amor::slotBubbleTimeout()
{
    // has the queue item been displayed for long enough?
    QueueItem *first = mTipsQueue.head();
#ifdef DEBUG_AMOR
    if (!first)	kDebug(10000) << "Amor::slotBubbleTimeout(): empty queue!";
#endif
    if ((first->time() > BUBBLE_TIME_STEP) && (mBubble->isVisible()))
    {
    	first->setTime(first->time() - BUBBLE_TIME_STEP);
        mBubbleTimer->setSingleShot(true);
	mBubbleTimer->start(BUBBLE_TIME_STEP);
	return;
    }

    // do not do anything if the mouse pointer is in the bubble
    if (mBubble->mouseWithin())
    {
	first->setTime(500);		// show this item for another 500ms
    mBubbleTimer->setSingleShot(true);
	mBubbleTimer->start(BUBBLE_TIME_STEP);
	return;
    }

    // are there any other tips pending?
    if (mTipsQueue.count() > 1)
    {
	mTipsQueue.dequeue();
	showBubble();	// shows the next item in the queue
    } else
	hideBubble(true); // hideBubble calls dequeue() for itself.
}

//===========================================================================

AmorSessionWidget::AmorSessionWidget()
{
    // the only function of this widget is to catch & forward the
    // saveYourself() signal from the session manager
    connect(kapp, SIGNAL(saveYourself()), SLOT(wm_saveyourself()));
}

void AmorSessionWidget::wm_saveyourself()
{
    // no action required currently.
}


// kate: word-wrap off; encoding utf-8; indent-width 4; tab-width 4; line-numbers on; mixed-indent off; remove-trailing-space-save on; replace-tabs-save on; replace-tabs on; space-indent on;
// vim:set spell et sw=4 ts=4 nowrap cino=l1,cs,U1: